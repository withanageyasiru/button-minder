// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Description:
// ------------
// An Arduino program for the Digispark board revision B that simulates a short 
// pressing of an expternal 5V pull down momentary switch (aka 'target switch') on
// an external board (aka 'target board') that is connected to the Digispark board.
//
// Digispark board connections:
// ----------------------------
// GND -> GND of target board.
// +5V -> +5V of target board.
// P2  -> Target button (must be a 5V pull up button).
//
// Operation:
// ----------
// When the Digispark board start it it's P2 is in high Z mode except for a short
// pull down pulse that simulates pressing the target button. The functionality
// of the Digispark board can be toggled on/off by long pressing the target button
// while powering the Digispark board. The on/off setting is stored in the 
// Digispark's EEPROM.
//
// Notes:
// -----
// * The stock Digispark board comes with a bootloader that has a 5 seconds delay
//   from the time power is applied until this program starts. It is recomanded to
//   program the alternative Digispark bootloader (the 'jumper' version) that 
//   avoids this delay.

// TODO: why do we need to include it here? It is included in eeprom_settings.cpp.
#include <EEPROM.h>

#include "debouncer.h"
#include "eeprom_settings.h"
#include "led_pattern.h"
#include "passive_timer.h"

// Digispark onboard LED. Used for diagnostics, active high.
#define LED_PIN 1

// Pin P2 of the Digispark is used to sense (as andlog input) and to active (as 
// digital output) the target button. Even though both functions uses the same pin,
// analog and digital I/Os have different pin numberings.
# define BUTTON_PIN_AS_DIGITAL 2
# define BUTTON_PIN_AS_ANALOG 1

// Debouncing period in millis for the target button sensing input. We will 
// consider the target button to be at a given state only after it is stable for 
// this time period.
#define BUTTON_DEBOUNCE_MILLIS 100

// The time in millis for determining a long target button press. This is the long
// press that toggle the setting of this board.
#define BUTTON_LONG_PRESS_MILLIS 5000

// If the button debouncer cannot get a stable reading within this  time priod in 
// milliseconds, the program enters the fatal error state and stays passive. 
// Something must be wrong.
#define BUTTON_DEBOUNCING_TIMEOUT 5000

// The current led pattern. The actual led value is computed by ledPattern() based
// on time_in_state and this pattern; The led pattern define the led on/off states
// over 32 time slot of one second cycles. See led_pattern.h for more details.
static unsigned long led_pattern;

// Read the state of the target button. Requires that the button pin is in input
// mode. Return true IFF target button is pressed. This is a pre debouncing value.
boolean readButtonPin() {
  // Full scale reading of 1023 represents 5V, so 200 is about 1V.
  return analogRead(BUTTON_PIN_AS_ANALOG) < 200;
}

// The program is modeled as a finite state machine with these states.
typedef enum {
  // Initial state. Handles the detection of the optional target button's long 
  // press that toggles the settings in the EEPROM. 
  // Transitions to STATE_PRESS_TARGET_BUTTON if needs to 'press' the target button
  // or to STATE_IDLE otherwise.
  STATE_IS_LONG_PRESS,
  // Press target button by issuing a open collector pulse. Transitions to 
  // STATE_IDLE.
  STATE_PRESS_TARGET_BUTTON,
  // Final state. Does nothing.
  STATE_IDLE,
  // If an error occured, the device stays in this passive state until turned off.
  // It issue fast bursts of LED pulses to indicate the error state.
  STATE_FATAL_ERROR
} 
State;

// The current state of the program.
static State current_state;

// The program is modeled as a finite state machine. This timer tracks the time in
//  millis the program is in the current state.
static PassiveTimer time_in_state;

// Utility function to initialize a new state. 
// TODO: why making the arg State instead of int does not compile?
void enterState(int state) {
  current_state = (State) state;
  time_in_state.restart(); 
  led_pattern = 0x00000000;
}

// --- State handlers forward declaration.

struct StateIsLongPress {
  void enter();
  void handle();
private:
  Debouncer button_debouncer_;
  // We set this to true if we detected a long press.
  boolean long_press_detected_;
} 
state_is_long_press;

struct StatePressTargetButton {
  void enter();
  void handle();
} 
state_press_target_button;

struct StateIdle {
  void enter();
  void handle();
} 
state_idle;

struct StateFatalError {
  void enter();
  void handle();
} 
state_fatal_error;

// --- IS_LONG_PRESS state handler implementation

void StateIsLongPress::enter() {
  pinMode(BUTTON_PIN_AS_DIGITAL, INPUT); 
  enterState(STATE_IS_LONG_PRESS);
  button_debouncer_.restart(BUTTON_DEBOUNCE_MILLIS); 
  long_press_detected_ = false; 
}

void StateIsLongPress::handle() {
  const int t = time_in_state.time_millis();

  // Read button state and update the debouncer.
  button_debouncer_.update(readButtonPin());  

  // Handle the case were decouncing has not stabalized yet.
  if (!button_debouncer_.hasStableValue()) {
    if (t > BUTTON_DEBOUNCING_TIMEOUT) {
      state_fatal_error.enter();
      return;
    }  
    // Keep waiting for stablizaton;
    digitalWrite(LED_PIN, ledPattern(t, 0x00010001)); 
    return;
  }

  // Here, the button tracker has a stable value. 

  // If the button is released then  then we can transition to the next state.
  if (!button_debouncer_.stableValue()) {
    if (EepromSettings::read()) {
      state_press_target_button.enter();
    } 
    else {
      state_idle.enter();
    }
    return;
  }

  // Here when the button is pressed, see if we just detected a long press.
  if (button_debouncer_.millisInStableValue() >= BUTTON_LONG_PRESS_MILLIS && 
    !long_press_detected_) {
    long_press_detected_ = true;

    // Flip the eeprom flag.
    boolean old_flag = EepromSettings::read();
    EepromSettings::write(!old_flag);
    if (EepromSettings::read() == old_flag) {
      // Failed to toggle.
      state_fatal_error.enter();
      return;
    }
  }

  led_pattern = long_press_detected_ ? 0x00010101 : 0x00000015; 
}


// ---  PRESS_TARGET_BUTTON state handler implementation 

//struct PressTargetButton {
void StatePressTargetButton::enter() {
  pinMode(BUTTON_PIN_AS_DIGITAL, INPUT); 
  enterState(STATE_PRESS_TARGET_BUTTON);
}

void StatePressTargetButton::handle() {
  const int t = time_in_state.time_millis();

  // TODO: make these number #define consts.
  //
  // We simulate a 300ms button press, starting 600ms after 
  // entering this state to make it easier to notice it on
  // the diagnostics LED>
  if (t >= 600 && t <= 900) { 
    // This 'presses' the target button. Repeating it over and over in
    // handle() calls does no harm (verified the signal with an osciloscope).
    pinMode(BUTTON_PIN_AS_DIGITAL, OUTPUT); 
    digitalWrite(BUTTON_PIN_AS_DIGITAL, LOW);
    led_pattern = 0xffffffff;
  } 
  else {
    // Make the output passive.
    pinMode(BUTTON_PIN_AS_DIGITAL, INPUT); 
    led_pattern = 0x00000000;
  }

  // Exit the state after 2sec. We could exit eariler but this make the
  // diagnostics LED easier to understand.
  if (t > 2000) {
    state_idle.enter();
  }
}

// --- IDLE state handler implementation

//struct StateIdle {
void StateIdle::enter() {
  pinMode(BUTTON_PIN_AS_DIGITAL, INPUT); 
  enterState(STATE_IDLE); 
  led_pattern = 0x00000001; 
}

void StateIdle::handle() {
}


// --- FATAL_ERROR state handler implementation

void StateFatalError::enter() {
  pinMode(BUTTON_PIN_AS_DIGITAL, INPUT); 
  enterState(STATE_FATAL_ERROR);
  led_pattern = 0x00550055;
}

void StateFatalError::handle() {
}

// --- Main

void setup() { 
  pinMode(LED_PIN, OUTPUT); 
  digitalWrite(LED_PIN, LOW);
  pinMode(BUTTON_PIN_AS_DIGITAL, INPUT); 
  state_is_long_press.enter();
}

void loop() {
  // Update diagnostic based on pattern and time.
  digitalWrite(LED_PIN, 
  ledPattern(time_in_state.time_millis(), led_pattern) ? HIGH : LOW);

  // Service current state.
  switch (current_state) {
  case STATE_IS_LONG_PRESS:
    state_is_long_press.handle();
    break;
  case STATE_PRESS_TARGET_BUTTON:
    state_press_target_button.handle();
    break;
  case STATE_IDLE:
    state_idle.handle();
    break;
  case STATE_FATAL_ERROR:
    state_fatal_error.handle();
    break;
  default:
    state_fatal_error.enter();
    break;
  }   
}





