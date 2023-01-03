/**
VGArduino - VGA video card on an Arduino Uno.
Inspired by Ben Eater's VGA card on breadboards.
Very loosely based on Nick Gammon's work at http://www.gammon.com.au/forum/?id=11608.
Resolution: 40x30
Pinouts:
- D0-D1: Serial
- D2: Unused
- D3: HSYNC
- D4: VSYNC
- D5-D7: Color outputs

This is running Snake.
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

#define HORIZ_PIXELS 40 // it's 40x30 so...
#define HORIZ_DRAW 8 // start draw @ (4us sync + 2us bp)/0.5us timerspeed
#define HORIZ_DRAW_END 54
enum modes {
  BLNK = 0b00000000, // nothing
  SYNC = 0b00010000, // vsync pin
  DRAW = 0b11100111  // enable drawing
};

//framebuffer
#define FRAMEBUFFER_INIT_MASK 0b11100000
byte lineData[30][40];

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
  DDRD |= 0b11111000; // set pins D2-D7 to output (not setting serial pins for reasons)
  
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
  
  user_init(); 
  sei();
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
  register byte* data = &(lineData[(dLine++)>>4][0]);
  //delayMicroseconds(1);
  while(TCNT2 < HORIZ_DRAW_END) PORTD = *data++;
  PORTD = BLNK;
  //return data - &(lineData[(dLine - 1)>>4][0]);
}

void loop() {
  sleep_mode(); // make sure to sleep so we start up predictably
  if(vLine == VERTICAL_FP) update();
  if(vLine == 0) draw();
}

void updateFramebufferSolid(byte val){
  enableDraw = 0x00;
  wait_line(0);
  for(byte i = 0; i < 30; i++){
    for(byte j = 0; j < 40; j++){
      lineData[i][j] = val & FRAMEBUFFER_INIT_MASK;
    }
  }
  enableDraw = 0xFF;
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
  while(i < 40)
    lineData[Ypos][i++] = *updateBuffer++;
  enableDraw = 0xFF;
}

void updateFramebufferBlock(byte Ystart, byte Yend, byte Xstart, byte Xend, byte val){
  wait_line(0);
  enableDraw = 0x00;
  if(Yend > 29) Yend = 29;
  if(Xend > HORIZ_PIXELS) Xend = HORIZ_PIXELS;
  while(Ystart < Yend){
    for(byte i = Xstart; i < Xend; ++i){
      lineData[Ystart][i] = val;
    }
    ++Ystart;
  }
}

// User program space

// define directional buttons
#define UP 3
#define DOWN 2
#define LEFT 1
#define RIGHT 0

// define colors used
#define PLAYER 0b01000000
#define APPLE 0b10000000

byte currentDirection = 0b00;
byte updateDirection = 0b00; // this exists because you can't turn 180 degrees

#define DELAY_STAGES 15
byte currentStage = 0x0;

struct vec2d {
  byte x;
  byte y;
};

struct{
  vec2d head;
  vec2d tail;
  unsigned int tailOffset = 4;
  bool dead = false;
} player;

struct{
  byte x;
  byte y;
  bool isGathered = true;
} apple;

#define DIRECTION_COUNT 1600

byte directionsMoved[400];
unsigned int dirMovedIndex = 0;

byte getDirection(unsigned int offset){
  byte temp = (offset % 4) * 2;
  return (directionsMoved[offset/4] & (0b11 << temp)) >> temp;
}

void setDirection(unsigned int offset, byte val){
  byte temp = (offset % 4) * 2;
  directionsMoved[offset/4] &= (0b11 << temp)^0xFF;
  directionsMoved[offset/4] |= val << temp;
}

void decodeDirection(struct vec2d* val, byte direction){
  if(direction == LEFT) --val->x;
  if(direction == RIGHT) ++val->x;
  if(direction == UP) --val->y;
  if(direction == DOWN) ++val->y;
}

byte inputBuffer = 0b0000;

void user_init(){
  // runs in void setup(), helper function
  DDRB &= 0b11110000;
  PORTB |= 0b00001111; // set PB0-PB3 to pullup
  srand(analogRead(0)); // init rand
  updateFramebufferSolid(0x00);
  for(auto& i : directionsMoved){
    i = 0;
  }
  player.head = {19, 14};
  player.tail = {15, 14};
}

void update(){
  // use update() for code to handle input
  // this runs in vertical front porch so it should be reasonably fast
  // DO NOT CLEAR INTERRUPTS
  inputBuffer = PINB^0b00001111;
  if(inputBuffer & bit(UP)) updateDirection = UP;
  if(inputBuffer & bit(DOWN)) updateDirection = DOWN;
  if(inputBuffer & bit(LEFT)) updateDirection = LEFT;
  if(inputBuffer & bit(RIGHT)) updateDirection = RIGHT;
}

void draw(){
  // use draw() for code that refreshes the framebuffer or other code like that
  // this runs in vertical back porch so it has a bit more time to run
  // DO NOT CLEAR INTERRUPTS
  if(player.dead){
    return;
  }else{
    // make game run at a usable speed
    if(++currentStage == DELAY_STAGES){
      currentStage = 0;
      // make sure that player cannot backtrack (ie, move down while moving up)
      if(updateDirection >>1 != currentDirection >>1) currentDirection = updateDirection;
      if(true) dirMovedIndex = (++dirMovedIndex) % DIRECTION_COUNT;
      // update player head, kill if hit edge of screen
      decodeDirection(&player.head, currentDirection);
      if(player.head.x > 39 || player.head.y > 29) player.dead = true;
      setDirection((dirMovedIndex + player.tailOffset) % DIRECTION_COUNT, currentDirection);
      // erase bits
      decodeDirection(&player.tail, getDirection(dirMovedIndex));
      // self collision
      if(lineData[player.head.y][player.head.x] == PLAYER) player.dead = true;
      if(lineData[player.head.y][player.head.x] == APPLE){
        apple.isGathered = true;
        ++player.tailOffset;
        decodeDirection(&player.tail, getDirection(dirMovedIndex--)^0b01);
      }
      // draw everything
      if(!apple.isGathered) updateFramebufferPixel(apple.y, apple.x, APPLE);
      if(!player.dead){
        updateFramebufferPixel(player.head.y, player.head.x, PLAYER);
        updateFramebufferPixel(player.tail.y, player.tail.x, 0x00);
      }
    }
    if(apple.isGathered){
      apple.x = rand() % 40;
      apple.y = rand() % 30;
      if(lineData[apple.y][apple.x] != PLAYER) apple.isGathered = false;
    }
  }
}