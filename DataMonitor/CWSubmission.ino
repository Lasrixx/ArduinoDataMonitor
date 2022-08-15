#include <Wire.h>
#include <Adafruit_RGBLCDShield.h>
#include <utility/Adafruit_MCP23017.h>
#include <EEPROM.h>
#include <TimerOne.h>

Adafruit_RGBLCDShield lcd = Adafruit_RGBLCDShield();

#define ID F("ID: F128451")
#define EXTENSIONS F("UDCHARS,FREERAM,HCI,NAMES,EEPROM,SCROLL,RECENT")

//#define DEBUG
#ifdef DEBUG
#define debug_print(description,value) Serial.print(description); Serial.println(value)
#else
#define debug_print(description,value)
#endif

// UDCHARS
#define UP_ARROW 0
#define DOWN_ARROW 1

// UDCHARS
byte up_arrow[] = { B00100, B01110, B11111, B11111, B00100, B00100, B00100, B00000 };
byte down_arrow[] = { B00000, B00100, B00100, B00100, B11111, B11111, B01110, B00100 };


// Define a 'custom data type' that will act as one record of the array of channels
typedef struct chnl {
  char ch_name;
  char ch_descr[16];
  byte ch_val;
  byte ch_recent_vals[4];
  byte ch_recent_avgs[8];
  byte vals_pointer;
  byte avgs_pointer;
  bool avgs_reset = false;
  // By default, the min=0, max=255, everything else set manually
  byte ch_min = 0;
  byte ch_max = 255;
  bool below_min = false;
  bool above_max = false;
  bool val_set = false;
} t_channel;

enum state_e { INITIALISATION = 0, AWAITING_INPUT, AWAITING_RELEASE, SYNCHRONISATION };

volatile bool scroll = false;

void setup() {
  Serial.begin(9600);
  lcd.begin(16, 2);
  lcd.setBacklight(7);
  lcd.clear();

  // SCROLL
  Timer1.initialize(500000);         // initialize timer1, and set a 1/2 second period
  Timer1.attachInterrupt(call_scroll);  // attaches call_scroll() as a timer overflow interrupt

  // Create custom arrow characters (UDCHARS)
  lcd.createChar(UP_ARROW, up_arrow);
  lcd.createChar(DOWN_ARROW, down_arrow);
}

void update_display(int ch_idx, t_channel channels[], byte next_empty_channel, bool min_shown, bool max_shown, bool scrolling_up) {

  /*
    This functions purpose is to display the arrows, channel ID, channel description on the lcd
    Also copes with displaying channels with values beyond maximum or minimum values (HCI)

    Parameters:
      ch_idx: the index of channel in the channels array that will be displayed
      channels: array of all channels
      next_empty_channel: the index that would next have a new channel put into it. This gives an idea of the how many structs are in channels array
      min_shown: represents if the user has pressed the left button to display all channels with a value below its minimum (HCI)
      max_shown: represents if the user has pressed the right button to display all channels with a value above its maximum (HCI)
  */

  // If the min or max lists are being displayed, then skip over indexes of channels
  int next_ch_idx = ch_idx + 1;
  int upper_bound = next_empty_channel - 2;
  int lower_bound = 0;
  if (min_shown) {
    // Amends the designated first index of channels and last index of channels to only span the channels with out of range values
    upper_bound = get_min_upper_bound(channels, next_empty_channel);
    lower_bound = get_min_lower_bound(channels, next_empty_channel);
    debug_print(F("upper: "), upper_bound);
    debug_print(F("lower: "), lower_bound);
    next_ch_idx = get_channel_display_indices(channels, &ch_idx, next_empty_channel, min_shown, max_shown, scrolling_up);
  }
  else if (max_shown) {
    // Same idea for channels outside the maximum
    upper_bound = get_max_upper_bound(channels, next_empty_channel);
    lower_bound = get_max_lower_bound(channels, next_empty_channel);
    debug_print(F("upper: "), upper_bound);
    debug_print(F("lower: "), lower_bound);
    next_ch_idx = get_channel_display_indices(channels, &ch_idx, next_empty_channel, min_shown, max_shown, scrolling_up);
  }
  // Display the channels:
  // Check whether the arrows should be displayed (removed if at top or bottom of list)
  debug_print(F("display index: "), ch_idx);
  lcd.clear();
  lcd.setCursor(0, 0);
  // UDCHARS
  // if a bound is -1 this implies that there are no channels with out of range values
  // so do not display arrows
  if (ch_idx > lower_bound && lower_bound != -1) {
    lcd.write(UP_ARROW);
  }
  lcd.setCursor(0, 1);
  if (next_ch_idx < upper_bound + 1 && upper_bound != -1 && next_ch_idx != -1) {
    lcd.write(DOWN_ARROW);
  }
  // Print channel values
  // Only print channel if it has been created (without, prints null values)
  if (channels[ch_idx].ch_name != 0 && ch_idx != -1) {
    lcd.setCursor(1, 0);
    lcd.print(channels[ch_idx].ch_name);
    if (channels[ch_idx].val_set) {
      display_value(channels[ch_idx].ch_val, 0, 2);
      lcd.setCursor(5, 0);
      lcd.print(",");
      display_value(get_avg(channels[ch_idx]), 0, 6);
    }
    // NAMES
    lcd.setCursor(10, 0);
    lcd.print(channels[ch_idx].ch_descr);
  }
  if (channels[next_ch_idx].ch_name != 0 && next_ch_idx != -1) {
    lcd.setCursor(1, 1);
    lcd.print(channels[next_ch_idx].ch_name);
    if (channels[next_ch_idx].val_set) {
      display_value(channels[next_ch_idx].ch_val, 1, 2);
      lcd.setCursor(5, 1);
      lcd.print(",");
      display_value(get_avg(channels[next_ch_idx]), 1, 6);
    }
    // NAMES
    lcd.setCursor(10, 1);
    lcd.print(channels[next_ch_idx].ch_descr);
  }
}

void display_value(byte val, byte row, byte col) {
  /*
    Positions the channel's value depending on how big it is.
    Values should be right-aligned.

    Parameters:
      val: the value to be aligned and printed
      row: the row of the lcd that the value should be printed on (depends on whether channel is in top row or bottom row of lcd)
      col: the starting column of the LCD that the value is being printed on
  */
  if (val >= 100) {
    lcd.setCursor(col, row);
  }
  else if (val >= 10) {
    lcd.setCursor(col + 1, row);
  }
  else {
    lcd.setCursor(col + 2, row);
  }
  lcd.print(val);
}

int get_min_lower_bound(t_channel channels[], int next_empty_channel) {
  /*
    Searches through the channels array forwards until it finds the first value
    where the channel's value is lower than its minimum boundary

    Parameters:
      channels: array of all channels
      next_empty_channel: the index that would next have a new channel put into it. This gives an idea of the how many structs are in channels array
  */
  for (int i = 0; i < next_empty_channel; i++) {
    if (check_below_min(channels[i])) {
      return i;
    }
  }
  return -1;
}

int get_min_upper_bound(t_channel channels[], int next_empty_channel) {
  /*
    Searches through the channels array backwards until it finds the first value
    where the channel's value is lower than its minimum boundary

    Parameters:
      channels: array of all channels
      next_empty_channel: the index that would next have a new channel put into it. This gives an idea of the how many structs are in channels array
  */
  for (int i = next_empty_channel - 1; i >= 0; i--) {
    if (check_below_min(channels[i])) {
      return i - 1;
    }
  }
  return -1;
}

int get_max_lower_bound(t_channel channels[], int next_empty_channel) {
  /*
    Searches through the channels array forwards until it finds the first value
    where the channel's value is higher than its maximum boundary

    Parameters:
      channels: array of all channels
      next_empty_channel: the index that would next have a new channel put into it. This gives an idea of the how many structs are in channels array
  */
  for (int i = 0; i < next_empty_channel; i++) {
    if (channels[i].ch_val > channels[i].ch_max) {
      return i;
    }
  }
  return -1;
}

int get_max_upper_bound(t_channel channels[], int next_empty_channel) {
  /*
    Searches through the channels array backwards until it finds the first value
    where the channel's value is higher than its maximum boundary

    Parameters:
      channels: array of all channels
      next_empty_channel: the index that would next have a new channel put into it. This gives an idea of the how many structs are in channels array
  */
  for (int i = next_empty_channel - 1; i >= 0; i--) {
    if (channels[i].ch_val > channels[i].ch_max) {
      return i - 1;
    }
  }
  return -1;
}

bool check_below_min(t_channel channel) {
  /*
    Checks if the given channel's value is below its minimum boundary

    Parameters:
      channel: the channel being checked
  */
  debug_print(F("checking min"), ' ');
  if (channel.ch_val < channel.ch_min && channel.val_set) {
    return true;
  }
  return false;
}

bool check_above_max(t_channel channel) {
  /*
    Checks if the given channel's value is above its maximum boundary

    Parameters:
    channel: the channel being checked
  */
  debug_print(F("checking max"), ' ');
  if (channel.ch_val > channel.ch_max && channel.val_set) {
    return true;
  }
  return false;
}

int get_channel_display_indices(t_channel channels[], int *ch_idx, int next_empty_channel, bool min_shown, bool max_shown, bool scrolling_up) {
  /*
    Used when the minimum or maximum channels are being displayed (HCI) to calculate what indices minimum or maximum channels
    occupy. This allows the arduino to display minimum and maximum channels as though they were next to each other
    in the channels array, even though they may be opposite ends of the array.

    Parameter:
      channels: array of all channels in the program
       ch_idx: pointer to the channel being displayed on the top row of the LCD, this is used as a start point to find the next channel to display
      next_empty_channel: an indicator of the length of channels that is occupied
      min_shown: shows if only minimum channels are being displayed currently
      max_shown: shows if only maximum channels are currently being displayed
      scrolling_up: shows if the button UP was pressed (need to reverse order of search for indices)
  */
  // Initialise next_ch_idx as -1, which represents that a second channel fitting the criteria does not exist
  int next_ch_idx = -1;
  if (min_shown) {
    bool ch_idx_exists = false;
    if (!scrolling_up){
      for (int i = *ch_idx; i < next_empty_channel ; i++) {
        if (check_below_min(channels[i])) {
          // Find the first channel in the array (from the current channel being displayed) that is below its minimum
          *ch_idx = i;
          ch_idx_exists = true;
          break;
        }
      }
    }
    else{
      for (int i = *ch_idx; i >= 0; i--) {
        if (check_below_min(channels[i])) {
          // Find the first channel in the array (from the current channel being displayed) that is below its minimum
          *ch_idx = i;
          ch_idx_exists = true;
          break;
        }
      }      
    }
    debug_print(F("ch_idx: "), *ch_idx);
    if (ch_idx_exists == false) {
      debug_print(F("no channels are below minimum"), ' ');
      *ch_idx = -1;
    }
    else {
      // If ch_idx is given a value this means at least one channel exists that matches the criteria, so it is possible another channel exists too
      // Now, we search for it
      for (int i = *ch_idx + 1; i < next_empty_channel ; i++) {
        if (check_below_min(channels[i])) {
          next_ch_idx = i;
          break;
        }
      }
      debug_print(F("next_ch_idx: "), next_ch_idx);
    }
  }
  else if (max_shown) {
    // Same idea for channels outside the maximum
    bool ch_idx_exists = false;
    if (!scrolling_up) {
      for (int i = *ch_idx; i < next_empty_channel ; i++) {
        if (check_above_max(channels[i])) {
          *ch_idx = i;
          ch_idx_exists = true;
          break;
        }
      }
    }
    else {
      for (int i = *ch_idx; i >= 0; i--) {
        if (check_above_max(channels[i])) {
          *ch_idx = i;
          ch_idx_exists = true;
          break;
        }
      }
    }
    debug_print(F("ch_idx: "), *ch_idx);
    if (ch_idx_exists == false) {
      debug_print(F("no channels are above maximum"), ' ');
      *ch_idx = -1;
    }
    else {
      for (int i = *ch_idx + 1; i < next_empty_channel ; i++) {
        if (check_above_max(channels[i])) {
          next_ch_idx = i;
          break;
        }
      }
      debug_print(F("next_ch_idx: "), next_ch_idx);
    }
  }
  return next_ch_idx;
}

void set_backlight(t_channel channels[], byte channel_amount) {
  /*
    Checks the values of all channels and sets the lcd's backlight accordingly

    Parameters:
      channels: array of all channels
  */

  debug_print(F("backlight"), ' ');

  bool below_range = false;
  bool above_range = false;
  for (int i = 0; i < channel_amount; i++) {
    if (channels[i].below_min && channels[i].ch_min <= channels[i].ch_max) {
      // Sets backlight to green if any channel's value is below its minimum value
      lcd.setBacklight(2);
      below_range = true;
    }
    if (channels[i].above_max) {
      // Sets backlight to red if any channel's value is above its maximum value
      lcd.setBacklight(1);
      above_range = true;
    }
    if (below_range && above_range) {
      // Sets backlight to yellow if there exists at least one channel below its minimum
      // and at least one channel with a value above its maximum
      lcd.setBacklight(3);
      break;
    }
  }
  if (!below_range && !above_range) {
    // Sets backlight to white if all channel values are between their minimum-maximum ranges
    lcd.setBacklight(7);
  }
}

bool check_format(String inp) {
  /*
    Validates the input coming through the Serial monitor and discards any that do not fit the accepted inputs

    Parameters:
      inp: the input that needs to be validated
  */
  // Any V, X, N command longer than 5 must be wrong (e.g. VA123 is longest command you can have)
  if (inp.length() > 5) {
    print_error(inp);
    return false;
  }
  // Check channel id is is in range [A-Z]
  if (!isupper(inp[1])) {
    print_error(inp);
    return false;
  }
  // Check value, min, max is [0-255]
  inp.remove(0, 2);
  if (inp.toInt() < 0 || inp.toInt() > 255) {
    print_error(inp);
    return false;
  }
  return true;
}

void print_error(String inp) {
  /*
    Prints an error to the Serial monitor along with the input that is erroneous

    Parameters:
      inp: input that generated the error
  */
  Serial.print(F("ERROR:"));
  Serial.println(inp);
}

void scroll_button_pressed(int btn_pressed, int *current_channel, t_channel channels[], int next_empty_channel, bool min_shown, bool max_shown) {
  /*
    Deal with the up or down button being pressed, and stopping the user being able to
    scroll beyond the channels that have been created

    Parameters:
      btn_pressed: represents which button was pressed last
       current_channel: pointer to the index of channels that is currently being displayed on the lcd
      channels: array of all channels
      next_empty_channel: index in channels that will be occupied next in the next channel creation; also gives length of channels
      min_shown: represents whether the lcd should display only channels where its value is lower than its minimum boundary
      max_shown: representes whether the lcd should display only channels where its value is higher than its minimum boundary
  */
  // Set up the upper and lower bound values (if min or max is shown these values are unlikley to be 0
  // and the end of the list so need to be manually calculate the boundaries)
  int lower_bound = 0;
  int upper_bound = next_empty_channel - 2;
  int next_ch_idx = *current_channel + 1;
  if (min_shown) {
    // Amends the designated first index of channels and last index of channels to only span the channels with out of range values
    upper_bound = get_min_upper_bound(channels, next_empty_channel);
    lower_bound = get_min_lower_bound(channels, next_empty_channel);
    next_ch_idx = get_channel_display_indices(channels, current_channel, next_empty_channel, min_shown, max_shown, false);
  }
  else if (max_shown) {
    lower_bound = get_max_lower_bound(channels, next_empty_channel);
    upper_bound = get_max_upper_bound(channels, next_empty_channel);
    next_ch_idx = get_channel_display_indices(channels, current_channel, next_empty_channel, min_shown, max_shown, false);
  }
  // Now deal with button presses and calling the necessary functions
  if (btn_pressed == BUTTON_UP && *current_channel > lower_bound && *current_channel != -1) {
    update_display(--*current_channel, channels, next_empty_channel, min_shown, max_shown,true);
    if (max_shown || min_shown){
      // Update the current channel being displayed in top row for the rest of the program
      next_ch_idx = get_channel_display_indices(channels, current_channel, next_empty_channel, min_shown, max_shown, true);
    }
  } else if (btn_pressed == BUTTON_DOWN && next_ch_idx < upper_bound + 1 && next_ch_idx != -1) {
    update_display(++*current_channel, channels, next_empty_channel, min_shown, max_shown,false);
  }
}

void select_button_pressed() {
  /*
    Displays student ID number (F128451) and the amount of free SRAM when the select button is held
  */
  lcd.setCursor(0, 0);
  lcd.print(ID);
  lcd.setCursor(0, 1);
  //FREERAM
  int freeMem = get_free_ram();
  lcd.print("Free SRAM: " + String(freeMem) + 'B');
}

//FREERAM
int get_free_ram() {
  /*
    Calculates the amount of free SRAM left in the Arduino. This is calculated
    by subtracting the combined total of memory taken up by stack and heap
    memory from the total amount of memory available in the Arduino
  */
  extern int __heap_start;  // Refers to the variable storing the start of the heap memory in an external file
  extern int *__brkval; // Refers to the pointer for memory in an external file
  int free_mem; // The amount of free SRAM which will be calculated

  if (__brkval == 0) { //If stack memory is empty, then the amount of free SRAM is the amount of memory not taken up by the heap memory
    free_mem = (int)&free_mem - (int)&__heap_start;
  }
  else {
    free_mem = (int)&free_mem - (int)__brkval; //If stack memory is not empty, then the amount of free memory is what is left after the pointer
  }
  return free_mem;
}

// EEPROM
int get_channel_address_EEPROM(char channel_name) {
  /*
    Finds the address in EEPROM that a channel with a given ID is stored at.
    This allows us to overwrite previous values stored in there (e.g. a new minimum boundary).

    Parameters:
      channel_name: the ID (A-Z) of the channel we need the address of
  */
  // Read how many channels are stored in EEPROM
  // Then go through each channel and compare its ID to the ID we are after
  // Return its address index
  int address = 0;
  byte channel_count = EEPROM.read(address);
  address += 2;
  for (int i = 0; i < channel_count; i++) {
    // IDs are stored at address 1,21,41,61,...
    char id = EEPROM.read(address);
    if (id == channel_name) {
      // If the channel does exist we can return its starting address
      return address;
    }
    address += 20;
  }
  // If the channel does not exist we can return the next available EEPROM address
  return channel_count * 20 + 2;
}

void write_channel_to_EEPROM(int address, t_channel channel) {
  /*
    Write data to EEPROM for the channel given. This could write a whole new channel to EEPROM,
    or update the values of a pre-existing channel.

    Parameters:
      address: the address in EEPROM of the channel's ID. ID is the first piece of data stored for a channel so indicates its location
      channel: the channel we are writing to EEPROM
  */
  // Update channel ID in EEPROM - needed if a new channel is created
  debug_print(F("ID: "), address);
  EEPROM.update(address, channel.ch_name);

  address += 1;

  // Write the channel's description to EEPROM
  // Need to extend the description to 15 characters long by filling space at the end with spaces
  // This prevents the EEPROM reading values written from previous programs
  String ch_descr = channel.ch_descr;
  byte len = ch_descr.length();
  if (len < 15) {
    for (int i = len; i < 15; i++) {
      ch_descr = ch_descr + " ";
    }
  }
  EEPROM.update(address, len);
  address += 1;
  for (int i = 0; i < 15; i++)
  {
    address++;
    EEPROM.update(address, ch_descr[i]);
  }

  address += 1;

  // Write channel's minimum boundary to EEPROM
  EEPROM.update(address, channel.ch_min);

  address += 1;

  // Write channel's maximum boundary to EEPROM
  EEPROM.update(address, channel.ch_max);

  debug_print(F("Written"), ' ');
}

// SCROLL
void call_scroll() {
  // Set scroll to true so the description can be scrolled one to place to the left every half-second
  scroll = true;
}

// RECENT
byte get_avg(t_channel channel) {
  int avg_sum = 0;
  // Average all recent avgs and vals and print
  // Need to get estimate of previous averages -> avg = sum/count implies sum = avg*count
  for (byte i = 0; i < sizeof(channel.ch_recent_avgs) / sizeof(byte); i++) {
    if (i != channel.avgs_pointer || channel.vals_pointer != 0) {
      avg_sum += channel.ch_recent_avgs[i] * sizeof(channel.ch_recent_vals) / sizeof(byte);
    }
  }
  for (byte i = 0; i < channel.vals_pointer; i++) {
    // Add the values in the recent_vals array to the avg_sum
    avg_sum += channel.ch_recent_vals[i];
  }
  // Count depends on whether ch_recent_avgs has been filled before then reset
  // If it has not, then count will be how many indices of ch_recent_avgs are filled by a value * 4 + the vals_pointer value
  byte count = ((sizeof(channel.ch_recent_vals) / sizeof(byte)) * (channel.avgs_pointer) + channel.vals_pointer);
  if (channel.avgs_reset) {
    // Otherwise, we want to say 28 values (7*4) + vals_pointer
    count = ((sizeof(channel.ch_recent_vals) / sizeof(byte)) * (sizeof(channel.ch_recent_avgs) / sizeof(byte))) + channel.vals_pointer;
  }
  // Calculate average
  byte avg = avg_sum / count;
  return avg;
}

void loop() {
  static enum state_e state = INITIALISATION; // Store the current state in Finite State Machine
  static int btn_pressed; // Store which button was pressed
  static unsigned long press_time; // Store the time the button was pressed
  static unsigned long start_time; // Store the time the button was first pressed
  static int prev_btn = 0; // Store for the last button press
  static int current_channel = 0; // Store the index of the channel currently pointed to by the scroll
  static t_channel channels[26];  // Array of structs storing all information about the channels
  static int next_empty_channel = 0;  // Store the index of the next unused index of channels array
  static bool lcd_cleared;  // Bool to clear the lcd only once, instead of every loop
  static bool min_shown = false;  // Store whether the min list (HCI) is shown
  static bool max_shown = false;  // Store whether the max list (HCI) is shown
  static byte topPos = 0;
  static byte botPos = 0;

  switch (state) {
    case AWAITING_RELEASE:
      {
        // Governs the holding select feature
        if (millis() - start_time >= 1000) {
          if (btn_pressed == BUTTON_SELECT) {
            if (!lcd_cleared) {
              lcd.clear();
              lcd.setBacklight(5);
              lcd_cleared = true;
            }
            select_button_pressed();
            int btn = lcd.readButtons();
            int released = ~btn & prev_btn;
            prev_btn = btn; // Save
            if (released & btn_pressed) {
              update_display(current_channel, channels, next_empty_channel, min_shown, max_shown,false);
              lcd.setBacklight(7);
              state = AWAITING_INPUT;
            }
          }

        }
        // Governs the scrolling feature
        if (millis() - start_time >= 1500) {
          if (millis() - press_time >= 150) {
            // Timeout
            press_time = millis(); // Reset pressed time to current time
            scroll_button_pressed(btn_pressed, &current_channel, channels, next_empty_channel, min_shown, max_shown);
          }
          else {
            int btn = lcd.readButtons();
            int released = ~btn & prev_btn;
            prev_btn = btn; // Save
            if (released & btn_pressed) {
              state = AWAITING_INPUT;
            }
          }
        }
        else {
          if (millis() - press_time >= 350) {
            // Timeout
            press_time = millis();
            scroll_button_pressed(btn_pressed, &current_channel, channels, next_empty_channel, min_shown, max_shown);
          }
          else {
            int btn = lcd.readButtons();
            int released = ~btn & prev_btn;
            prev_btn = btn; // Save
            if (released & btn_pressed) {
              state = AWAITING_INPUT;
            }
          }
        }
        break;
      }
    case INITIALISATION:
      {
        // EEPROM
        //First value indicates how many channels are stored in EEPROM
        int address = 0;
        byte channel_amount = EEPROM.read(address);
        debug_print(F("Channels in EEPROM: "), channel_amount);
        address++;
        for (int i = 0; i < channel_amount; i++) {
          address++;
          channels[i].ch_name = EEPROM.read(address);
          address++;
          // Read channel's description
          int descr_len = EEPROM.read(address);
          char data[descr_len + 1];
          address++;
          for (int j = 0; j < 15; j++) {
            address++;
            data[j] = EEPROM.read(address);
          }
          data[descr_len] = '\0';
          strcpy(channels[i].ch_descr, data);
          address++;
          // Read channel's minimum boundary
          channels[i].ch_min = EEPROM.read(address);
          address++;
          // Read channel's maximum boundary
          channels[i].ch_max = EEPROM.read(address);

          debug_print(F("Channel ID: "), channels[i].ch_name);
          debug_print(F("Channel description: "), channels[i].ch_descr);
          debug_print(F("Channel min value: "), channels[i].ch_min);
          debug_print(F("Channel max value: "), channels[i].ch_max);

          next_empty_channel = channel_amount;

        }

        state = SYNCHRONISATION;
        break;
      }
    case AWAITING_INPUT:
      {
        // SCROLL
        if (scroll) {
          int next_channel_index = current_channel + 1;
          if (min_shown || max_shown) {
            // If min or max shown, get the next_channel_index as it is unlikely to be the next available channel
            next_channel_index = get_channel_display_indices(channels, &current_channel, next_empty_channel, min_shown, max_shown, false);
          }
          byte descr_length = 0;
          byte descr2_length = 0;
          // Calculate size of descr and descr2 that is actually occupied by description - looking for mull-terminating character
          if (current_channel != -1) {
            for (byte i = 0; i < sizeof(channels[current_channel].ch_descr) / sizeof(char); i++) {
              if (channels[current_channel].ch_descr[i] == '\0') {
                break;
              }
              descr_length++;
            }
          }
          if (next_channel_index != -1) {
            for (byte i = 0; i < sizeof(channels[next_channel_index].ch_descr) / sizeof(char); i++) {
              if (channels[next_channel_index].ch_descr[i] == '\0') {
                break;
              }
              descr2_length++;
            }
          }
          // If the description is more than 6 characters long, scroll the description
          if (descr_length > 6) {
            lcd.setCursor(10, 0);
            // Only scroll the description until the final character is displayed on the last position on the LCD screen
            if (topPos < descr_length - 5) {
              for (byte i = topPos; i < topPos + 6 ; i++) {
                // Print the subset of the descr array from position 10-15 on LCD
                lcd.print(channels[current_channel].ch_descr[i]);
              }
            }
            topPos++;
            if (topPos > descr_length - 4) {
              // If the end of the description has been reached, pause for a second, then reset the decription back to the start and scroll again
              topPos = 0;
            }
          }
          else {
            // If the description is less than 7 characters, there is no need to scroll it as it all fits
            lcd.setCursor(10, 0);
            if (current_channel != -1){
              lcd.print(channels[current_channel].ch_descr);
            }
            lcd.print(F("      "));
          }
          // Same logic for the bottom description
          if (descr2_length > 6) {
            lcd.setCursor(10, 1);
            if (botPos < descr2_length - 5) {
              for (byte i = botPos; i < botPos + 6 ; i++) {
                lcd.print(channels[next_channel_index].ch_descr[i]);
              }
            }
            botPos++;
            if (botPos > descr2_length - 4) {
              botPos = 0;
            }
          }
          else {
            lcd.setCursor(10, 1);
            if (next_channel_index != -1){
              lcd.print(channels[next_channel_index].ch_descr);
            }
            lcd.print(F("      "));
          }
          scroll = false;
        }
        if (Serial.available()) {
          // Read input from Serial monitor
          String inp = Serial.readStringUntil('\n');
          inp.trim();
          // Find the index of the channel with the given ID (if it exists)
          int existing_channel = -1;
          for (int i = 0; (unsigned)i < sizeof(channels) / sizeof(t_channel); i++) {
            if (channels[i].ch_name == inp[1]) {
              existing_channel = i;
            }
          }

          // Find channel address in EEPROM
          int address = get_channel_address_EEPROM(inp[1]);
          debug_print(F("channel address: "), address);

          switch (inp[0]) {
            case 'C':
              if (isupper(inp[1])) {
                if (existing_channel != -1) {
                  // If channel does exist
                  // Update description for channel (no need to overwrite channel name)
                  inp.remove(0, 2);
                  char buffer_descr[16];
                  inp.toCharArray(buffer_descr, 16);
                  strcpy(channels[existing_channel].ch_descr, buffer_descr);
                  // Update value in EEPROM
                  write_channel_to_EEPROM(address, channels[existing_channel]);
                }
                else {
                  // If channel does not exist
                  // Set channel name and description in the channel's alphabetically correct index
                  // Below, is an insertion algorithm to place new channels in their correct position
                  bool element_inserted = false;
                  // Case 1: Array is empty - add new channel in first index
                  if (channels[0].ch_name == 0) {
                    debug_print(F("first char"), ' ');
                    channels[0].ch_name = inp[1];
                    inp.remove(0, 2);
                    char buffer_descr[16];
                    inp.toCharArray(buffer_descr, 16);
                    strcpy(channels[0].ch_descr, buffer_descr);
                    element_inserted = true;
                  }
                  // Case 2: new channel ID < array's first ID - add new channel at front and push everything else back one index
                  if (inp[1] < channels[0].ch_name && !element_inserted) {
                    debug_print(F("at front"), ' ');
                    for (int i = sizeof(channels) / sizeof(t_channel) - 1; i >= 0; i--) {
                      if (i > 0) {
                        if (channels[i - 1].ch_name != 0) {
                          channels[i] = channels[i - 1];
                          // Need to specifically copy descriptions over as they are arrays
                          strcpy(channels[i - 1].ch_descr, channels[i].ch_descr);
                        }
                      }
                      else {
                        channels[0].ch_name = inp[1];
                        inp.remove(0, 2);
                        char buffer_descr[16];
                        inp.toCharArray(buffer_descr, 16);
                        strcpy(channels[0].ch_descr, buffer_descr);
                        // When a channel is created it has default values, but the values are left from previous channel so need to clear
                        channels[0].ch_val = 0;
                        channels[0].ch_min = 0;
                        channels[0].ch_max = 255;
                        channels[0].below_min = false;
                        channels[0].above_max = false;
                        channels[0].val_set = false;
                      }
                    }
                    element_inserted = true;
                  }
                  if (!element_inserted) {
                    for (int i = sizeof(channels) / sizeof(t_channel) - 1; i >= 0; i--) {
                      if (channels[i].ch_name != 0) {
                        // Case 3: new channel ID > array's ID - add new channel in i+1 and move everything else back an index
                        if (inp[1] > channels[i].ch_name) {
                          debug_print(F("inserting"), ' ');
                          for (int j = sizeof(channels) / sizeof(t_channel) - 1; j > i; j--) {
                            if (j > i + 1) {
                              if (channels[j - 1].ch_name != 0) {
                                channels[j] = channels[j - 1];
                                strcpy(channels[j - 1].ch_descr, channels[j].ch_descr);
                              }
                            }
                            else {
                              channels[i + 1].ch_name = inp[1];
                              inp.remove(0, 2);
                              char buffer_descr[16];
                              inp.toCharArray(buffer_descr, 16);
                              strcpy(channels[i + 1].ch_descr, buffer_descr);
                              // When a channel is created it has default values, but the values are left from previous channel so need to clear
                              channels[i + 1].ch_val = 0;
                              channels[i + 1].ch_min = 0;
                              channels[i + 1].ch_max = 255;
                              channels[i + 1].below_min = false;
                              channels[i + 1].above_max = false;
                              channels[i + 1].val_set = false;
                            }
                          }
                          element_inserted = true;
                          break;
                        }
                      }
                    }
                  }
                  // Update values in EEPROM
                  next_empty_channel++;
                  EEPROM.update(0, next_empty_channel);
                  for (byte i = 0; i < next_empty_channel; i++) {
                    int address = i * 20 + 2;
                    write_channel_to_EEPROM(address, channels[i]);
                  }
                }
                update_display(current_channel, channels, next_empty_channel, min_shown, max_shown,false);
              }
              else {
                print_error(inp);
              }
              break;
            case 'X':
              // Update max value for the given channel (set as 255 as default)
              if (existing_channel != -1 && check_format(inp)) {
                inp.remove(0, 2);
                channels[existing_channel].ch_max = inp.toInt();
                channels[existing_channel].below_min = check_below_min(channels[existing_channel]);
                channels[existing_channel].above_max = check_above_max(channels[existing_channel]);
                set_backlight(channels, next_empty_channel);
                update_display(current_channel, channels, next_empty_channel, min_shown, max_shown,false);
                write_channel_to_EEPROM(address, channels[existing_channel]);
              }
              else {
                print_error(inp);
              }
              break;
            case 'N':
              // Set minimum value for the given channel (default is 0)
              if (existing_channel != -1 && check_format(inp)) {
                inp.remove(0, 2);
                channels[existing_channel].ch_min = inp.toInt();
                channels[existing_channel].below_min = check_below_min(channels[existing_channel]);
                channels[existing_channel].above_max = check_above_max(channels[existing_channel]);
                set_backlight(channels, next_empty_channel);
                update_display(current_channel, channels, next_empty_channel, min_shown, max_shown,false);
                write_channel_to_EEPROM(address, channels[existing_channel]);
              }
              else {
                print_error(inp);
              }
              break;
            case 'V':
              // Set the current value for the given channel
              if (existing_channel != -1 && check_format(inp)) {
                inp.remove(0, 2);
                channels[existing_channel].ch_val = inp.toInt();
                channels[existing_channel].val_set = true;
                channels[existing_channel].below_min = check_below_min(channels[existing_channel]);
                channels[existing_channel].above_max = check_above_max(channels[existing_channel]);
                set_backlight(channels, next_empty_channel);
                // RECENT
                // Add new value to ch_recent_vals
                channels[existing_channel].ch_recent_vals[channels[existing_channel].vals_pointer] = inp.toInt();
                channels[existing_channel].vals_pointer++;
                // Check if val_pointer = 4, need to set back to 0, and average ch_recent_vals and put into ch_recent_avgs
                if (channels[existing_channel].vals_pointer > sizeof(channels[existing_channel].ch_recent_vals) / sizeof(byte) - 1) {
                  channels[existing_channel].vals_pointer = 0;
                  int sum = 0;
                  for (byte i = 0; i < sizeof(channels[existing_channel].ch_recent_vals) / sizeof(byte); i++) {
                    sum += channels[existing_channel].ch_recent_vals[i];
                  }
                  // Calculate average of last 4 values and add to avgs array
                  byte avg = sum / sizeof(channels[existing_channel].ch_recent_vals) / sizeof(byte);
                  channels[existing_channel].ch_recent_avgs[channels[existing_channel].avgs_pointer] = avg;
                  channels[existing_channel].avgs_pointer++;
                  if (channels[existing_channel].avgs_pointer > sizeof(channels[existing_channel].ch_recent_avgs) / sizeof(byte) - 1) {
                    channels[existing_channel].avgs_pointer = 0;
                    // Need to set avgs_reset when the avgs array is filled and pointer moves back to 0
                    // Because now when we calculate overall average, we need to know all of avgs is filled but not to use a given index
                    // When that index is being currently filled by new values
                    channels[existing_channel].avgs_reset = true;
                  }
                }
                update_display(current_channel, channels, next_empty_channel, min_shown, max_shown,false);
              }
              else {
                print_error(inp);
              }
              break;
            default:
              // If no match has been found then command given is invalid - print error
              print_error(inp);
              break;
          }
        }
        int btn = lcd.readButtons();
        int pressed = btn & ~prev_btn;
        prev_btn = btn;
        if (pressed & (BUTTON_UP | BUTTON_DOWN)) {
          // If up or down is pressed, need to scroll the LCD screen if possible and move to AWAITING_RELEASE
          scroll_button_pressed(pressed, &current_channel, channels, next_empty_channel, min_shown, max_shown);
          btn_pressed = pressed;
          press_time = millis();
          start_time = millis();
          state = AWAITING_RELEASE;
        }
        else if (pressed & BUTTON_SELECT) {
          // Move to AWAITING_RELEASE, after being held for a second, display student ID number
          lcd_cleared = false;
          btn_pressed = pressed;
          start_time = millis();
          state = AWAITING_RELEASE;
        }
        else if (pressed & BUTTON_LEFT) {
          // Toggles minimum channels (HCI)
          lcd.clear();
          if (!min_shown) {
            min_shown = true;
            max_shown = false;
            // Need to set current_channel back to 0 to stop out-of-index errors occuring after switching from main display to min display
            current_channel = 0;
            update_display(current_channel, channels, next_empty_channel, min_shown, max_shown,false);
          }
          else {
            min_shown = false;
            current_channel = 0;
            update_display(current_channel, channels, next_empty_channel, min_shown, max_shown,false);
          }
        }
        else if (pressed & BUTTON_RIGHT) {
          // Toggles maximum channels (HCI)
          lcd.clear();
          if (!max_shown) {
            max_shown = true;
            min_shown = false;
            current_channel = 0;
            update_display(current_channel, channels, next_empty_channel, min_shown, max_shown,false);
          }
          else {
            max_shown = false;
            current_channel = 0;
            update_display(current_channel, channels, next_empty_channel, min_shown, max_shown,false);
          }
        }
        break;
      }
    case SYNCHRONISATION:
      {
        // Sends 'Q' to the Serial Monitor until 'X' has been recieved
        // This is to sync up the Python program with the Serial interface
        lcd.setBacklight(5);
        Serial.print(F("Q"));
        String inp = Serial.readString();
        inp.trim();
        if (inp == "X") {
          Serial.println(EXTENSIONS);
          lcd.setBacklight(7);
          update_display(0, channels, next_empty_channel, false, false,false);
          state = AWAITING_INPUT;
        }
        delay(1000);
        break;
      }
  }
}
