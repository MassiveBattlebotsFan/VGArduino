/**
VGArduino - VGA video card on an Arduino Uno.
Inspired by Ben Eater's VGA card on breadboards.
Very loosely based on Nick Gammon's work at http://www.gammon.com.au/forum/?id=11608.
Resolution: 44x30
Pinouts:
- D0-D1: Serial
- D2: Unused
- D3: HSYNC
- D4: VSYNC
- D5-D7: Color outputs

This is running an example serial program.
**/

#include <avr/pgmspace.h>
#include <avr/io.h>  
#include <avr/sleep.h> // gotta config sleep mode for later:tm:
#include <TimerHelpers.h>

// sync pins
#define VSYNC 4
#define HSYNC 3

#define LED 13 % 8 //onboard LED, mod 8 for PORTB bit offset

/*
Vertical lines: 525
VSYNC length: 2
Back porch length: 33
Draw length: 480
Front porch length: 10
*/

#define VERTICAL_END 523 // we need this to reset vLine and start vsync
#define VERTICAL_BP 525 // enter back porch
#define VERTICAL_DRAW 33 // enter draw area, actually output pixels
#define VERTICAL_FP 513 // enter front porch
#define VERTICAL_SAFE VERTICAL_DRAW - 3 // end of update safe zone
/*
Horiz. pixels: 800
HSYNC length: 96
Back porch length: 48
Draw length: 640
Front porch length: 16

We don't have enough speed for this tho...
*/

#define HORIZ_PIXELS 44 // it's 44x30 so...
#define HORIZ_DRAW 8 // start draw @ (4us sync + 2us bp)/0.5us timerspeed
#define HORIZ_DRAW_END 53
enum modes {
  BLNK = 0b00000000, // nothing
  SYNC = 0b00010000, // vsync pin
  DRAW = 0b11100111  // enable drawing
};

//framebuffer
#define FRAMEBUFFER_INIT_MASK 0b11100000
byte lineData[30][44];

//macros for ease of use
#define wait_line(line) while(vLine > line)
#define is_draw_safe() (vLine < VERTICAL_SAFE)
//volatile byte hasResp = 0xff;
//volatile int messageLine; // unused
//volatile byte backPorchLinesToGo;

volatile byte enableDraw = 0x00;
volatile byte mode = BLNK;
volatile word vLine = 0;
word dLine = 0;

// declare helper functions
void draw();
void update();
void user_init();

void setup() {
  cli();
  //randomSeed(analogRead(0)); // init random
  // configure all of the pins
  DDRD |= 0b11111010; // set pins D2-D7 to output (not setting serial pins for reasons)
  PORTB |= bit(0); // set PB0 to pullup
  
  TIMSK0 = 0;  // no interrupts on Timer 0
  OCR0A = 0;   // and turn it off
  OCR0B = 0;
  TIMSK1 = 0;
  OCR1A = 0;
  OCR1B = 0;
  
  Timer2::setMode (7, Timer2::PRESCALE_8, Timer2::CLEAR_B_ON_COMPARE);
  OCR2A = 63;   // 32 / 0.5 µs = 64 (less one)
  OCR2B = 7;    // 4 / 0.5 µs = 8 (less one)
  TIFR2 = bit (TOV2);   // clear overflow flag
  TIMSK2 = bit (TOIE2);  // interrupt on overflow on timer 2

  set_sleep_mode(SLEEP_MODE_IDLE);
  
  sei();
  user_init();
  enableDraw = 0xFF;
}

ISR(TIMER2_OVF_vect){
  // hsync pulse wakes system
  if(mode == DRAW) drawLine(); 
  else PORTD = mode;

  switch(++vLine){
    case VERTICAL_FP:
      dLine = 0;
      mode = BLNK;
      break;
    case VERTICAL_BP:
      mode = BLNK;
      vLine = 0;
      break;
    case VERTICAL_DRAW:
      mode = DRAW & enableDraw;
      break;
    case VERTICAL_END:
      mode = SYNC;
      break;
  }
}

void drawLine(){
  register byte idx = dLine>>4;
  register byte* data = &(lineData[idx][0]);
  while(TCNT2 < 8);
  while(TCNT2 < HORIZ_DRAW_END) PORTD = *data++;
  PORTD = BLNK;
  dLine++;
}

void loop() {
  sleep_mode(); // make sure to sleep so we start up predictably
  if(vLine == VERTICAL_FP) update();
  if(vLine == 0) draw();
}

void updateFramebufferPixel(byte Ypos, byte Xpos, byte val){
  wait_line(30);
  lineData[Ypos][Xpos] = val & FRAMEBUFFER_INIT_MASK;
}

void updateFramebufferLine(byte Ypos, byte* updateBuffer){
  // updateBuffer should be a 44 byte array
  wait_line(0);
  enableDraw = 0x00; // disable drawing just in case
  byte i = 0;
  while(i < 44)
    lineData[Ypos][i++] = *updateBuffer++;
  enableDraw = 0xFF;
}

// User program space

byte fbUpdateBuffer[44];
byte fbUpdateBufferPos = 0;
byte verticalLineUpdatePos = 0;
byte tmp;

void user_init(){
  // runs in void setup(), helper function
  DDRB |= bit(LED);
  PORTB &= 0xFF ^ bit(LED); // set LED off for startup

  Serial.begin(115200);
  
  for(byte i = 0; i < 30; i++){
    for(byte j = 0; j < 44; j++){
      lineData[i][j] = random()&FRAMEBUFFER_INIT_MASK;
    }
  }

  while(!Serial.availableForWrite());
  Serial.println("Startup");
  delay(3000);

  while(!Serial.available());
  while(Serial.available()) Serial.read(); // clear serial buffer
  
  Serial.println("Running");
  
  PORTB |= bit(LED); // set LED on
}

void update(){
  // use update() for code to handle input
  // this runs in vertical front porch so it should be reasonably fast
  // DO NOT CLEAR INTERRUPTS
  if(fbUpdateBufferPos > 43){
    updateFramebufferLine(verticalLineUpdatePos++, &(fbUpdateBuffer[0]));
    fbUpdateBufferPos = 0;
    verticalLineUpdatePos %= 30;
  }
}

void draw(){
  // use draw() for code that refreshes the framebuffer or other code like that
  // this runs in vertical back porch so it has a bit more time to run
  // DO NOT CLEAR INTERRUPTS
  while(is_draw_safe() && Serial.available() && fbUpdateBufferPos < 44){
    tmp = (byte)Serial.read();
    fbUpdateBuffer[fbUpdateBufferPos++] = tmp << 5;
  }
}