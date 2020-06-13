/**
 * @file gdbstubs.cpp
 * @author Fernando Trias
 * @brief Implement GDB stub using TeensyDebug interface
 * @version 0.1
 * @date 2020-06-09
 * 
 * @copyright Copyright (c) 2020 Fernando Trias
 * 
 */
#include <Arduino.h>

#define CPU_RESTART_ADDR (uint32_t *)0xE000ED0C
#define CPU_RESTART_VAL 0x5FA0004
#define CPU_RESTART (*CPU_RESTART_ADDR = CPU_RESTART_VAL);

#define GDB_DEBUG_INTERNAL
#include "TeensyDebug.h"

/**
 * Code to communicate with GDB. Use standard nomenclature.
 * devInit() is not standard. It must be called at initialization.
 * 
 */
// #include <HardwareSerial.h>
// #define dev Serial1

Stream *dev = NULL;

/**
 * @brief Get the next character from the serial
 * 
 * @return int Character or -1 if error
 */
int getDebugChar() {
  unsigned int timeout = millis() + 1000;
  while(dev->available() <= 0) {
    delay(1);
    if (millis() > timeout) {
      // Serial.println("{timeout}");
      return -1;
    }
  }
  char c = dev->read();
  // Serial.print("{");Serial.print(c);Serial.print("}");
  return c;
}

/**
 * @brief Send a character to the serial
 * 
 * @param c Character to send (one 8-bit byte)
 */
void putDebugChar(int c) {
  // Serial.print("[");Serial.print((char)c);Serial.print("]");
  dev->write(c);
}


/**
 * @brief Return 1 or more if there is data to read
 * 
 * @return int Number of characters to read
 */
int hasDebugChar() {
  // Serial.println("has?");
  return dev->available();
}

/**
 * @brief Initialize serial
 * 
 */
void devInit(Stream *device = NULL) {
  if (device) {
    dev = device;
  }
  else if (dev == NULL) {
    dev = &Serial;
    Serial.begin(9600);
  }
}


// Signal codes for ARM faults
const char *signal_text[] = {
  "S05", "S02", "S10", "S11", "S11", "S10", "S04"
};

/**
 * @brief Calculate checksum for message
 * 
 * @param c Packet
 * @return int Checksum
 */
int calcChecksum(const char *c) {
  uint8_t sum = 0;
  while(*c) {
    sum += *c++;
  }
  return sum;
}

// constants for hex conversions
char int2hex[] = { '0','1','2','3','4','5','6','7','8','9','a','b','c','d','e','f' };

/**
 * @brief Convert a hex char to a number
 * 
 * @param ch Hex char '0', '1', etc.
 * @return int Number 0-15
 */
int hex(unsigned char ch) {
  if (ch >= 'a' && ch <= 'f')
    return ch - 'a' + 10;
  if (ch >= '0' && ch <= '9')
    return ch - '0';
  if (ch >= 'A' && ch <= 'F')
    return ch - 'A' + 10;
  return -1;
}

/**
 * @brief Take a memory span and format it in hexadecimal
 * 
 * @param buff Output in ascii hexadecimal
 * @param addr Memory address to convert
 * @param sz Number of bytes to convert
 * @return char* The address of the last character (a \0) in buff
 */
char *mem2hex(char *buff, const void *addr, int sz) {
  for (int i = 0; i < sz; i++) {
    uint8_t b = ((uint8_t*)addr)[i];
    *buff++ = int2hex[b >> 4];
    *buff++ = int2hex[b & 0x0F];
  }
  *buff = 0;
  return buff;
}

/**
 * @brief Convert hex string to actual string
 * 
 * @param buff Store string here
 * @param hexstr Hexadeciamal representation
 * @return char* Pointer to \0 at end of buff
 */
char *hex2str(char *buff, const char *hexstr) {
  while (*hexstr) {
    int c_high = hex(*hexstr++);
    int c_low = hex(*hexstr++);
    *buff++ = (c_high << 4) + c_low;
  }
  *buff = 0;
  return buff;
}

/**
 * While we find nice hex chars, build an int.
 * Return number of chars processed.
 */

/**
 * @brief Convert ascii hex into an integer. Used when parsing commands.
 * 
 * @param ptr Pointer to (char*) with text. Updated as text is parsed.
 * @param intValue Pointer to (int) that holds value parsed
 * @return int Number of characters parsed
 */
static int hexToInt(const char **ptr, int *intValue)
{
  int numChars = 0;
  int hexValue;

  *intValue = 0;

  while (**ptr) {
    hexValue = hex(**ptr);
    if (hexValue < 0)
      break;

    *intValue = (*intValue << 4) | hexValue;
    numChars++;

    (*ptr)++;
  }

  return (numChars);
}

static int hex32ToInt(const char **ptr)
{
  const char *p = *ptr;
  uint32_t intValue;
  uint8_t *i = (uint8_t*)&intValue;

  i[0] = (hex(p[0]) << 4) + hex(p[1]);
  i[1] = (hex(p[2]) << 4) + hex(p[3]);
  i[2] = (hex(p[4]) << 4) + hex(p[5]);
  i[3] = (hex(p[6]) << 4) + hex(p[7]);

  // Serial.print("parse ");Serial.println(intValue);
  *ptr += 8;
  return intValue;
}

/**
 * @brief Send result text to GDB (formatting and calculating checksum)
 * 
 * @param result String of message to send
 */
void sendResult(const char *result) {
  int checksum = calcChecksum(result);
  const char *presult = result;
  putDebugChar('$');
  while (*presult) {
    putDebugChar(*presult++);
  }
  putDebugChar('#');
  putDebugChar(int2hex[checksum >> 4]);
  putDebugChar(int2hex[checksum & 0x0F]);
  // Serial.println(result);
}

// global flag to enabling or suspending debug system
volatile int debug_active = 1;

// global flag indicating debug should is simulating a "halt"; set to 0 to continue
volatile int halt_state = 0;

// global flag indicating Ctrl-C, causing system to "break" as soon as possible
volatile int cause_break = 0;

// main routine for processing GDB commands and states
void processGDB();

// from debug class indicating a fault
extern int debug_id;

// from debug indicting we are "stepping" instructions
extern int debugstep;


extern int debugenabled;

// for messages that are sent seperately (like 'O', print)
char send_message[256];

/**
 * @brief Output to the GDB console using 'O' command
 * 
 * @param msg Message to display
 * @param len Number of characters
 * @return size_t Number of characters sent
 */
size_t gdb_out_write(const uint8_t *msg, size_t len) {
  if (send_message[0]) {
    int lx = strlen(send_message);
    char *p = send_message + lx;
    mem2hex(p, (const char *)msg, len);
  }
  else {
    send_message[0] = 'O';
    mem2hex(send_message+1, (const char *)msg, len);
  }
  return len;
}

/**
 * @brief Output string to GDB console using 'O'
 * 
 * @param msg String to print
 * @return size_t Number of characters sent
 */
size_t gdb_out_print(const char *msg) {
  return gdb_out_write((const uint8_t *)msg, strlen(msg));
}

/**
 * @brief Routing for processing breakpoints
 * 
 */
#pragma GCC push_options
#pragma GCC optimize ("O0")
void process_onbreak() {
  // send the signal
  sendResult(signal_text[debug_id]);
  // go into halt state and stay until flag is cleared
  halt_state = 1;
  while(halt_state) {
    delay(10);
    yield();
  }
  debug_id = 0;
}
#pragma GCC pop_options

/**
 * @brief Add 32-bit ascii hexadecimal to string buffer
 * 
 * @param p Buffer to hold ascii hex
 * @param n Number to encode
 * @return char* Pointer last item (a \0) so you can continue appending
 */
char *append32(char *p, uint32_t n) {
  uint8_t *x = (uint8_t *) &n;
  for(int i=0; i<4; i++) {
    int c = x[i];
    *p++ = int2hex[c >> 4];
    *p++ = int2hex[c & 0x0F];
  }
  return p;
}

/**
 * @brief Process 'g' command to return registers
 * 
 * @param cmd Original command
 * @param result String with encoded text to return
 * @return int 0 = success
 */
int process_g(const char *cmd, char *result) {
  result = append32(result, debug.getRegister("r0"));
  result = append32(result, debug.getRegister("r1"));
  result = append32(result, debug.getRegister("r2"));
  result = append32(result, debug.getRegister("r3"));
  result = append32(result, debug.getRegister("r4"));
  result = append32(result, debug.getRegister("r5"));
  result = append32(result, debug.getRegister("r6"));
  result = append32(result, debug.getRegister("r7"));
  result = append32(result, debug.getRegister("r8"));
  result = append32(result, debug.getRegister("r9"));
  result = append32(result, debug.getRegister("r10"));
  result = append32(result, debug.getRegister("r11"));
  result = append32(result, debug.getRegister("r12"));
  result = append32(result, debug.getRegister("sp"));;
  result = append32(result, debug.getRegister("lr"));
  result = append32(result, debug.getRegister("pc"));
  result = append32(result, debug.getRegister("cpsr"));
  *result = 0;
  return 0;
}

/**
 * @brief Process 'G' to write registers. Not supported.
 * 
 * @param cmd 
 * @param result 
 * @return int 
 */
int process_G(const char *cmd, char *result) {
  // Serial.print("command G:");Serial.println(cmd);
  cmd++;
  debug.setRegister("r0", hex32ToInt(&cmd));
  debug.setRegister("r1", hex32ToInt(&cmd));
  debug.setRegister("r2", hex32ToInt(&cmd));
  debug.setRegister("r3", hex32ToInt(&cmd));
  debug.setRegister("r4", hex32ToInt(&cmd));
  debug.setRegister("r5", hex32ToInt(&cmd));
  debug.setRegister("r6", hex32ToInt(&cmd));
  debug.setRegister("r7", hex32ToInt(&cmd));
  debug.setRegister("r8", hex32ToInt(&cmd));
  debug.setRegister("r9", hex32ToInt(&cmd));
  debug.setRegister("r10", hex32ToInt(&cmd));
  debug.setRegister("r11", hex32ToInt(&cmd));
  debug.setRegister("r12", hex32ToInt(&cmd));
  debug.setRegister("sp", hex32ToInt(&cmd));
  debug.setRegister("lr", hex32ToInt(&cmd));
  debug.setRegister("pc", hex32ToInt(&cmd));
  debug.setRegister("cspr", hex32ToInt(&cmd));
  strcpy(result, "OK");
  return 0;
}

/**
 * @brief Test is requested address is valid. GDB sometimes sends 
 * invalid memory requests that will cause a fault. Should test more
 * accurately, but this takes care of most problems.
 * 
 * @param addr Address to check
 * @return int 1 = valid; 0 = invalid
 */
int isValidAddress(uint32_t addr) {
  if (addr <= 0x20) {
    return 0;
  }
  else if (addr > 0xF0000000) {
    return 0;
  }
  return 1;
}

/**
 * @brief Process 'm' to read memory
 * 
 * @param cmd Original command
 * @param result String to return
 * @return int 0 for success
 */
int process_m(const char *cmd, char *result) {
  int addr, sz=4;

  cmd++; // skip cmd
  hexToInt(&cmd, &addr);
  if (*cmd == ',') {
    cmd++; // skip comma
    hexToInt(&cmd, &sz);
  }

  // Serial.print("read at ");Serial.println(addr, HEX);

  if (isValidAddress(addr) == 0) {
    strcpy(result, "E01");
    return 0;
  }

  uint8_t *m = (uint8_t *)addr;
  for (int i=0; i<sz; i++) {
    uint8_t d = m[i];
    *(result++) = int2hex[d >> 4];
    *(result++) = int2hex[d & 0x0F];
  }
  *result = 0;
  return 0;
}

/**
 * @brief Process 'M' to write memory
 * 
 * @param cmd Original command
 * @param result Results 'OK'
 * @return int 
 */
int process_M(const char *cmd, char *result) {
  int addr, sz;
  cmd++; // skip command
  hexToInt(&cmd, &addr);
  cmd++; // skip comma
  hexToInt(&cmd, &sz);
  cmd++;

  uint8_t *memory = (uint8_t*)addr;
  for(int i=0; i<sz; i++) {
    int c_high = hex(*cmd++);
    int c_low = hex(*cmd++);
    memory[i] = (c_high << 4) + c_low;
  }
  strcpy(result, "OK");
  return 0;
}

/**
 * @brief Process 'c' continue
 * 
 * @param cmd Original command
 * @param result String result
 * @return int 1 to signal caller
 */
int process_c(const char *cmd, char *result) {
  halt_state = 0; // not halted
  debugstep = 0;  // not stepping
  strcpy(result, "");
  return 1;
}

/**
 * @brief Process 's' step command to step a single instruction. 
 * Arguments to start in a different address are not supported.
 * 
 * @param cmd Original command
 * @param result String with ENN or blank
 * @return int 1 to continue; 0 if errors
 */
int process_s(const char *cmd, char *result) {
  cmd++;
  if (*cmd) {
    // int addr;
    // hexToInt(&cmd, &addr);
    // we don't support starting in a different address
    strcpy(result, "E01"); // SNN
    return 0;
  }
  debugstep = 1;   // just step
  halt_state = 0;
  strcpy(result, ""); // return comes from the actual break
  return 1;
}

/**
 * @brief Process '?' query for last stop reason. TODO.
 * 
 * @param cmd 
 * @param result 
 * @return int 
 */
int process_question(const char *cmd, char *result) {
  // sprintf(result, "S0%d", debug_id);
  strcpy(result, signal_text[debug_id]);
  return 0;
}

/**
 * @brief 'B' deprecated and not supported
 * 
 * @param cmd 
 * @param result 
 * @return int 
 */
int process_B(const char *cmd, char *result) {
  strcpy(result, "E10");
  return 0;
}

/**
 * @brief Process 'z' clear breakpoint at address
 * 
 * @param cmd Original command
 * @param result Result is ENN or OK
 * @return int 0 if success
 */
int process_z(const char *cmd, char *result) {
  int btype, addr;
  cmd++;
  hexToInt(&cmd, &btype);
  cmd++;
  hexToInt(&cmd, &addr);
//  cmd++;
//  hexToInt(&cmd, &sz);
  if (addr == 0) {
    strcpy(result, "E01");
  }
  else if (debug.clearBreakpoint((void*)addr)) {
    strcpy(result, "E01");
    strcpy(result, "OK");
  }
  else {
    strcpy(result, "OK");
  }
  return 0;
}

/**
 * @brief Process 'Z' set breakpoint at address
 * 
 * @param cmd Original command
 * @param result Result is ENN or OK
 * @return int 
 */
int process_Z(const char *cmd, char *result) {
  int btype, addr;
  cmd++;
  hexToInt(&cmd, &btype); // don't care. We figure out what's best.
  cmd++;
  hexToInt(&cmd, &addr);
  // optional size not used because we only support Thumb
//  cmd++;
//  hexToInt(&cmd, &sz);
  if (addr == 0) {
    strcpy(result, "E01");
  }
  else if (debug.setBreakpoint((void*)addr)) {
    strcpy(result, "E01");
  }
  else {
    strcpy(result, "OK");
  }
  return 0;
}

char *getNextWord(char **text) {
  char *orig = *text;
  char *p = orig;
  while(*p) {
    if (*p == ' ' || *p == '(' || *p == ')' || *p == ',') {
      *p++ = 0;
      while(*p == ' ') p++;
      *text = p;
      return orig;
    }
    p++;
  }
  *text = 0;
  return orig;
}

int process_monitor(char *cmd, char *result) {
  char *place = cmd;
  char *word;
  word = getNextWord(&place);
  // Serial.print("command1 ");Serial.println(word);
  if (stricmp(word, "digitalWrite") == 0) {
    char *pin = getNextWord(&place);
    char *state = getNextWord(&place);
    int ipin = atoi(pin);
    int istate = atoi(state);
    pinMode(ipin, OUTPUT);
    digitalWrite(ipin, istate);
    strcpy(result, "OK"); 
    return 0;   
  }
  else if (stricmp(word, "digitalRead") == 0) {
    char *pin = getNextWord(&place);
    int ipin = atoi(pin);
    pinMode(ipin, INPUT);
    int v = digitalRead(ipin);
    char x[256];
    sprintf(x, "%d\n", v);
    mem2hex(result, (const char *)x, strlen(x));
    return 0;   
  }
  else if (stricmp(word, "analogWrite") == 0) {
    char *pin = getNextWord(&place);
    char *state = getNextWord(&place);
    int ipin = atoi(pin);
    int istate = atoi(state);
    pinMode(ipin, OUTPUT);
    analogWrite(ipin, istate);
    strcpy(result, "OK"); 
    return 0;   
  }
  else if (stricmp(word, "analogRead") == 0) {
    char *pin = getNextWord(&place);
    int ipin = atoi(pin);
    pinMode(ipin, INPUT);
    int v = analogRead(ipin);
    char x[256];
    sprintf(x, "%d\n", v);
    mem2hex(result, (const char *)x, strlen(x));
    return 0;   
  }
  strcpy(result, "");    
  return 0;
}

/**
 * @brief Process 'q' query command. For now report back only PacketSize.
 * 
 * @param cmd Original command
 * @param result Results or ""
 * @return int 0
 */
int process_q(const char *cmd, char *result) {
  if (strncmp(cmd, "qSupported", 10) == 0) {
    strcpy(result, "PacketSize=1024");
    return 0;
  }
  else if (strncmp(cmd, "qRcmd", 5) == 0) {
    char x[256];
    hex2str(x, cmd+6);
    return process_monitor(x, result);
  }
  strcpy(result, "");    
  return 0;
}

/**
 * @brief Functions are not supported, but would be very useful in the future
 * 
 * @param cmd Function specification
 * @param result 
 * @return int 
 */
int process_F(const char *cmd, char *result) {
  // Serial.println(cmd);
  strcpy(result, "");    
  return 0;
}

/**
 * @brief Process 'R' restart
 * 
 * @param cmd 
 * @param result 
 * @return int 
 */
int process_R(const char *cmd, char *result) {
  // _reboot_Teensyduino_();
  // _restart_Teensyduino_();
  CPU_RESTART;
  strcpy(result, "");    
  return 0;
}

/**
 * @brief Process a command by calling appropriate delegation function
 * 
 * @param cmd Command and parameters
 * @param result Result to send back
 * @return int 0
 */
int processCommand(const char *cmd, char *result) {
  switch(cmd[0]) {
    case 'g': return process_g(cmd, result);
    case 'G': return process_G(cmd, result);
    case 'm': return process_m(cmd, result);
    case 'M': return process_M(cmd, result);
    case 'c': return process_c(cmd, result);
    case 's': return process_s(cmd, result);
    case 'F': return process_F(cmd, result);
    case 'R': return process_R(cmd, result);
    case 'r': return process_R(cmd, result);
    
    case '?': return process_question(cmd, result);
//    case 'B': return process_B(cmd, result);
    case 'z': return process_z(cmd, result);
    case 'Z': return process_Z(cmd, result);
    case 'q': return process_q(cmd, result);
  }
  // if it's not listed above, it's not supported
  result[0] = 0;
  return 0;
}

/**
 * @brief Read input, if available and process any commands
 * 
 */
void processGDBinput() {
  char result[1024];

  // no data? do nothing
  if (! hasDebugChar()) return;

  int c = getDebugChar();

  if (c < 0) {
    // Serial.println("Error reading");
    return;
  }

  // GDB ack'd our last command; don't do anything yet with this
  if (c == '+') {
    // Serial.println("ACK");
    return;
  }

  // GDB had a problem with last command; should resend. TODO
  if (c == '-') {
    // Serial.println("NAK");
    return;
  }

  // User hit Ctrl-C or other break
  if (c == 0x03) {
    // Serial.println("Ctrl-C");
    cause_break = 1; // cause break later so we don't break internals
    return;
  }

  // If we don't have a valid start command, then something went wrong so
  // we just ignore it.
  if (c != '$') {
    // Serial.print("Bad char: ");
    // Serial.println((char)c, HEX);
    return; // wait for start char
  }

  // buffer to read command; matches our PacketSize
  const int cmd_max = 1024;
  char cmd[cmd_max];      // buffer
  char *pcmd = cmd;       // pointer to last char
  // int sum;               
  int checksum = 0;   // to store checksum

  // 2-second timeout
  while(1) {
    // read next char or timeout
    c = getDebugChar();

    if (c == -1) {
      // Serial.println("read error");
      putDebugChar('-');
      return;
    }

    if (c == '#') break; // checksum follows
    *pcmd++ = c;
    if (pcmd >= cmd+cmd_max) { // overrun
      pcmd = cmd;
    }
  }
  *pcmd = 0;
  
  // Serial.print("got command:");Serial.println(cmd);
  
  c = getDebugChar();
  checksum = hex(c) << 4;
  c = getDebugChar();
  checksum += hex(c);
  if (checksum != calcChecksum(cmd)) {
    // Serial.println("bad checksum");
    // Serial.println(calcChecksum(cmd), HEX);
    putDebugChar('-');
    return;
  }

  // all good, so ACK
  putDebugChar('+');
  
  int r = processCommand(cmd, result);
  // r == 1 means there are no results for now. A step or continue
  // don't return immediate results. Results are returned upon
  // hitting the break or successful step
  if (r==1) return;

  // toss results back to GDB
  sendResult(result);
}

/**
 * @brief Process GDB messages, including Break
 * 
 */
void processGDB() {
#if 0
  static unsigned int nexttick = millis() + 1000;
  if (millis() > nexttick) {
    // Serial.println("tick");
    gdb_out_print("ping");
    nexttick += 1000;
  }
#endif
  if (! debug_active) return;
  processGDBinput();
  if (send_message[0]) {
    // Serial.print("send ");Serial.println(send_message);
    sendResult(send_message);
    send_message[0] = 0;
  }
  if (cause_break) {
    // Serial.println("BREAK!!");
    cause_break = 0;
    debug_id = 1;
    // NVIC_SET_PENDING(IRQ_SOFTWARE); 
    asm volatile("svc 0x12");
  }
}

// void setup_main();

// Check for GDB commands periodically
IntervalTimer gdb_timer;

/**
 * @brief Initialize debug system
 * 
 * @param device Optional device that inherits from Stream; default is Serial
 */
void gdb_init(Stream *device) {
  send_message[0] = 0;
  devInit(device);
  gdb_timer.begin(processGDB, 5000);
  debug.setCallback(process_onbreak);
  debug_active = 1;

  // We could halt at startup, but maybe it's best to let user 
  // explicitly do so with a breakpoint(n)
  //    debug.setBreakpoint(setup_main, 1);
}
