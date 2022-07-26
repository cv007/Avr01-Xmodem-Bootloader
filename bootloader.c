/*-----------------------------------------------------------------------------
    xmodem-crc bootloader for avr0/1

    ATtiny3217 Curiosity Nano in use here

    --- [1] ---
    set user specified defines-
        cpu speed- 16 or 20MHz
        bootloader size
        uart baud rate

    --- [2] ---
    set fuse values as needed
        BOOTEND is calculated using define set above
        other fuses set as needed
        (the example is using mega4809 fuses, so change to fuses fr=or your mcu)

    --- [3] ---
    set led and sw pin values
        this bootloader uses a switch as one method to signal bootloader, so
        a switch is needed, and an led is used to notify bootloader status
        if some other scheme is desired, simply change the code to suite

    --- [4] ---
    set uart info
        if alternate pins are used, also fill in the UartAltPins function body
        to handle setting portmux to use the alternate pins
        a table is provided that maps out pins to uarts for all avr0/1

    --- [5] ---
    compile and program bootloader

-----------------------------------------------------------------------------*/



// --- [1] ---
#define FREQSEL     2           // OSC20M speed- 1==16MHz, 2=20MHz
#define BL_SIZE     2048        // value divisible by 256, used in FUSES
#define UART_BAUD   230400      // will be checked to see if possible
// ----------


//=============================================================================
// will be using clock PDIV of 2, so will get either 10MHz or 8MHz which
// allows higher speeds for uart but still within speed limits for 3.3v power
#define F_CPU       (FREQSEL==2 ? 10000000ul : 8000000ul)
//=============================================================================
// check for valid define values
#if FREQSEL != 2 && FREQSEL != 1
#error "FREQSEL required to be 1 or 2"
#endif
#if BL_SIZE % 256
#error "BL_SIZE needs to be divisible by 256"
#endif
#if (F_CPU*4/UART_BAUD) < 64
#error "UART_BAUD value is too high for cpu speed"
#endif
//=============================================================================

#include <avr/io.h>
#include <stdint.h>
#include <stdbool.h>
#include <util/delay.h>



// --- [2] ---
                //for an atmega4809, but close enough for the timy3217
FUSES           = {
                .WDTCFG = 0x00, // WDTCFG {PERIOD=OFF, WINDOW=OFF}
                .BODCFG = 0x00, // BODCFG {SLEEP=DIS, ACTIVE=DIS, SAMPFREQ=1KHZ, LVL=BODLEVEL0}
                .OSCCFG = FREQSEL, // OSCCFG {FREQSEL=20MHZ(2)/16MHZ(1), OSCLOCK=CLEAR}
                .SYSCFG0 = 0xF6, // SYSCFG0 {EESAVE=CLEAR, RSTPINCFG=GPIO, CRCSRC=NOCRC}
                .SYSCFG1 = 0xFF, // SYSCFG1 {SUT=64MS}
                .APPEND = 0, // APPEND
                //boot starts at 0, goes up to bootend, app starts at bootend
                .BOOTEND = BL_SIZE/256, // BOOTEND
                };
// ----------


                //pins type
                typedef struct {
                    PORT_t* port;
                    uint8_t pin;
                    uint8_t pinbm;
                    bool onVal;
                    }
pin_t           ;


// --- [3] ---
                //our pins- Led and Sw

                static const pin_t
Led             = { &PORTA, 3, 1<<3, 0 };
                static const pin_t
Sw              = { &PORTB, 7, 1<<7, 0 };
// ----------


                /*-----------------------------------------------------------
                    reference for uart pins, any avr0/1

                    -avr0 mega- (availability depends on mega0 pin count)
                             usart | 0   1   2   3
                              port | A   C   F   B
                    default pin tx | A0  C0  F0  B0
                    default pin rx | A1  C1  F1  B1
                  alternate pin tx | A4  C4  F4  B4
                  alternate pin rx | A5  C5  F5  B5

                  asuming only 1 usart will be used, to set alt pins-
                  PORTMUX.USARTROUTEA = 1<<(N*2); // N = 0-3


                    -avr0/1 tiny (usart0 only)-
                    default pin tx | B2
                    default pin rx | B3
                  alternate pin tx | A1
                  alternate pin rx | A2

                    -avr0/1 tiny 8pin (usart0 only)-
                    default pin tx | A6
                    default pin rx | A7
                  alternate pin tx | A1
                  alternate pin rx | A2

                  asuming only usart will be used, to set alt pins-
                  PORTMUX.CTRLB = 1;
                -----------------------------------------------------------*/


// --- [4] ---
                //uart info

                static USART_t* const
Uart            = &USART0;
                static const pin_t
UartTx          = { &PORTB, 2, 1<<2, 0 }; //onVal value unimportant
                static const pin_t
UartRx          = { &PORTB, 3, 1<<3, 0 }; //onVal value unimportant
                //set function to handle enabling the alternate pins if needed
                //else leave as a blank function
                static void
UartAltPins     (){} // { PORTMUX.USARTROUTEA = 1<<0; /*mega0 USART0 alt pins*/ }
// ----------


                // enums

                enum { //xmodem chars
X_NACK          = 0x15,
X_ACK           = 0x06,
X_SOH           = 0x01,
X_EOT           = 0x04,
X_PING          = 'C' //to host, C = xmodem-crc (NACK = normal xmodem)
                };
                enum {
X_DATA_SIZE     = 128
                };

                // constants

                static volatile uint8_t* const
eeLastBytePtr   = (volatile uint8_t*)EEPROM_END;
                static void* const
appStartAddr    = (void*)BL_SIZE;
                static volatile uint8_t* const
appMemStart     = (volatile uint8_t*)(MAPPED_PROGMEM_START|BL_SIZE);

                //vars

                uint8_t
xmodemData      [X_DATA_SIZE]; //storage for an xmodem data packet (always 128 in size)

                //functions

                static bool
swIsOn          ()
                {
                (&Sw.port->PIN0CTRL)[Sw.pin] = 0x08; //pullup on
                _delay_ms(1); //give pullup time before we check switch
                return (Sw.port->IN & (1<<Sw.pin)) == Sw.onVal<<Sw.pin;
                }

                static void
ledOn           ()
                {
                Led.port->DIRSET = Led.pinbm;
                if(Led.onVal) Led.port->OUTSET = Led.pinbm;
                else Led.port->OUTCLR = Led.pinbm;
                }

                static void
ledTog          ()
                {
                Led.port->DIRSET = Led.pinbm;
                Led.port->OUTTGL = Led.pinbm;
                }

                static void //software reset
softReset       () { CCP = 0xD8; RSTCTRL.SWRR = 1; }

                static void
nvmWrite        () { CCP = 0x9D; NVMCTRL.CTRLA = 3; } //ERWP

                static bool //we enabled falling edge sense, so any rx will set the rx intflag
isRxActive      () //will clear flag, so can also use to just clear flag
                {  //(in case needed more than once)
                bool flag = UartRx.port->INTFLAGS & UartRx.pinbm;
                UartRx.port->INTFLAGS = UartRx.pinbm; //clear
                return flag;
                }

                static bool //return true if we want to stay in bootloader
entryCheck      () { return *eeLastBytePtr == 0xFF || swIsOn() || *appMemStart == 0xFF; }

                static void
init            ()
                {
                CCP = 0xD8; CLKCTRL.MCLKCTRLB = 1; //prescale enable, div2 (8Mhz or 10Mhz)
                Uart->BAUD = F_CPU*4/UART_BAUD;
                Uart->CTRLB = 0xC0; //RXEN,TXEN
                UartTx.port->DIRSET = UartTx.pinbm; //output
                (&UartRx.port->PIN0CTRL)[UartRx.pin] = 0x08|0x03; //pullup, falling edge sense
                UartAltPins(); //function to handle alternate pins if needed
                }

                static void
uwrite          (const char c)
                {
                while( (Uart->STATUS & 0x20) == 0 ){} //DREIF
                Uart->TXDATAL = c;
                }

                static uint8_t
uread           ()
                {
                while ( (Uart->STATUS & 0x80) == 0 ){} //RXC
                return Uart->RXDATAL;
                }

                static void
dumpMem         (uint16_t addr, uint16_t size){
                uwrite( addr & 0xFF ); //header- addrL addrH sizeL sizeH
                uwrite( addr >> 8 );
                uwrite( size & 0xFF );
                uwrite( size >> 8 );
                volatile uint8_t* ptr = (volatile uint8_t*)addr; 
                while( size-- ) uwrite(*ptr++);
                }
                static void
dumpFlash       () { dumpMem( MAPPED_PROGMEM_START, MAPPED_PROGMEM_SIZE ); }
                static void
dumpEeprom      () { dumpMem( MAPPED_EEPROM_START, MAPPED_EEPROM_SIZE ); }
                static void
dumpFuses       () { dumpMem( FUSES_START, FUSE_MEMORY_SIZE ); }
                static void
dumpSigrow      () { dumpMem( (uint16_t)&SIGROW, sizeof(SIGROW_t) ); }

                static void
Xbroadcast      ()
                {
                //the other end expects a NACK or 'C' (xmodem crc) when we are ready
                //we do not know if sender is ready yet (may not see our C),
                //so to get things started send out a C (PING) every second or so until
                //we see the first rx start bit
                while(1){
                    ledTog(); //blink when waiting for sender
                    uwrite( X_PING );
                    uint32_t t = F_CPU/10; //count to wait (while loop about 10 clocks)
                    bool rx;
                    while( rx = isRxActive(), t-- && !rx ){}
                    if( rx ) break;
                    }
                }

                static uint16_t
crc16           (uint16_t crc, uint8_t v)
                {
                crc = crc ^ (v << 8);
                for( uint8_t i = 0; i < 8; i++ ){
                    bool b15 = crc & 0x8000;
                    crc <<= 1;
                    if (b15) crc ^= 0x1021;
                    }
                return crc;
                }

                static bool
xmodem          () //we let caller ack when its ready for more data
                {
                while(1){
                    uint8_t c;
                    uint16_t crc = 0;
                    c = uread();
                    if( c == X_EOT ) return false;
                    if( c != X_SOH ) continue;
                    //X_SOH seen
                    uint8_t blockSum = uread() + uread(); //block#,block#inv, sum should be 255
                    for( uint8_t i = 0; i < X_DATA_SIZE; i++ ){
                        uint8_t v = uread();
                        xmodemData[i] = v;
                        crc = crc16( crc, v );
                        }
                    if( crc == ((uread()<<8u) + uread()) && blockSum == 255 ) break;
                    uwrite( X_NACK ); //bad checksum or block# pair not a match
                    }
                return true;
                }

                static void
programApp      ()
                {
                Xbroadcast(); //let other end know we are here
                ledOn(); //on when xmodem active (probably will not see for very long)
                volatile uint8_t* flashPtr = appMemStart;
                while( xmodem() ){ //returns false when EOT seen
                    uint8_t i = 0;
                    uint8_t pbc = 0; //page buffer count
                    //also handle avr0/1 with page size < 128 (64 is the only other lower value)
                    while( i < X_DATA_SIZE ){ //128
                        flashPtr[i] = xmodemData[i]; //write to page buffer
                        i++;
                        if( ++pbc < MAPPED_PROGMEM_PAGE_SIZE ) continue;
                        nvmWrite(); //end of page, write page buffer
                        pbc = 0; //reset page buffer count
                        }
                    i = 0;
                    while( (flashPtr[i] == xmodemData[i]) && (++i < X_DATA_SIZE) ){} //verify
                    if( i != X_DATA_SIZE ){ uwrite( X_NACK ); continue; }
                    uwrite( X_ACK ); //ok
                    flashPtr += X_DATA_SIZE; //next page
                    //if flash write failure- instead of retrying flash write on our own (we already have the data),
                    //let the sender know there is an error so it is informed
                    //(it will send the data again, the sender will decide when/whether its time to give up)
                    }
                uwrite( X_ACK ); //ack the EOT
                }

                static void
eeAppOK         ()
                {
                *eeLastBytePtr = 0; //write to eeprom page buffer, last eeprom byte
                nvmWrite(); //write eeprom (!0xFF signifies to bootloader that flash is programmed)
                while( NVMCTRL.STATUS & 2 ){} //ee is busy
                }

                int
main            (void)
                {

                //check if bootloader needs to run, true=run, false=jump to app
                //convert BL_SIZE to flash address mapped into data space
                //goto will result in a jmp instruction so can use byte address
                if( entryCheck() == false ) goto *appStartAddr;

                //we are now officially a bootloader
                init();
                programApp();
                eeAppOK();              //mark that app is programmed
                dumpSigrow();           //dump sigrow, fuses, flash, eeprom
                dumpFuses();            //can use to verify flash or check
                dumpFlash();            //other things- device id, fuses, etc.
                dumpEeprom();           //
                while( swIsOn() ){}     //in case sw still pressed, wait for release
                softReset();

                while(1){}

                }
