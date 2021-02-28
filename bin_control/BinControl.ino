#include "esp_camera.h"
#include <WiFi.h>
#include <ESP32Servo.h>
#include <esp_http_client.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_PWMServoDriver.h>

//
// WARNING!!! Make sure that you have either selected ESP32 Wrover Module,
//            or another board which has PSRAM enabled
//

#define CAMERA_MODEL_AI_THINKER

#include "camera_pins.h"

const char* ssid = "iPhone 11 neo"; //"YXIN"; //"iPhone 11 neo";
const char* password = "11121111";

#define I2C_SDA 13
#define I2C_SCL 15
#define FREQUENCY 50
Adafruit_PWMServoDriver pwm1 = Adafruit_PWMServoDriver(&Wire, 0x40);
LiquidCrystal_I2C lcd(0x27,16,2);  // set the LCD address to 0x27 for a 16 chars and 2 line display

#define yes_button 4
#define no_button 2
#define sensorPin 14

// Change initial state to READY after sensor is added !!!!!
#define READY 0
#define PROCIMG 1
#define EXEC 2
int state = READY; // Whether its idle (READY), processing image (PROCIMG)
int action = 0;
int* top_3_cat = new int[3];

// Macros for action
#define DUMP 1
#define WAIT 2
#define WAITFOREVER 3

// Lookup for categories (same order as on computer)
const char* category_set[] = {
  "styrofoam", "aluminum can", "bottle", "cardboard",
  "paper", "paper box", "plastic bag", "plastic wrap"
};
char recyclability_set[] = {
  '0', '1', '1', '1',
  '1', '1', '0', '0'
};

// Global variables
char* response_buffer = new char[20]; // Exact buffer size to be determined
char* human_input_buffer = new char[2];

//void startCameraServer();
inline void print_line(LiquidCrystal_I2C lcd, const char* msg, boolean wipe);
int pulseWidth(int angle);

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();

  // Set up the pushbuttons
  pinMode(yes_button, INPUT);
  pinMode(no_button, INPUT);
  pinMode(sensorPin, INPUT);

  // Set up the servo
  pwm1.begin(I2C_SDA, I2C_SCL);
  pwm1.setPWMFreq(FREQUENCY);  // Analog servos run at ~50 Hz updates
  
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  //init with high specs to pre-allocate larger buffers
  if (psramFound()) {
    config.frame_size = FRAMESIZE_UXGA;
    config.jpeg_quality = 10;
    config.fb_count = 2;
  } else {
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
  }

  // camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }

  sensor_t * s = esp_camera_sensor_get();
  //initial sensors are flipped vertically and colors are a bit saturated
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);//flip it back
    s->set_brightness(s, 1);//up the blightness just a bit
    s->set_saturation(s, -2);//lower the saturation
  }
  //drop down frame size for higher initial frame rate
  s->set_framesize(s, FRAMESIZE_QVGA);
  
  lcd.init(); // initialize the lcd
  lcd.backlight();
  print_line(lcd, "Initialized", true); // Print initialization message on the LCD.

  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi connected");
  print_line(lcd, "WiFi connected", true);

  //  startCameraServer();
  s->set_framesize(s, FRAMESIZE_SXGA);
  s->set_quality(s, 20);

  Serial.print("Camera Ready! Use 'http://");
  Serial.print(WiFi.localIP());
  Serial.println("' to connect");
}

int pulseWidth(int angle) {
  int analogAngle = int(map(angle, 0, 90, 200, 400));
  return analogAngle;
}

/*
 * Custom print function for the lcd
 */
inline void print_line(LiquidCrystal_I2C lcd, const char* msg, boolean wipe) {
  if (wipe) {
    lcd.clear();
  }
  int i = 0;
  while (msg[i] != '\0') {
    if (i >= 16) {
      lcd.setCursor(0, 1);
    }
    lcd.print(msg[i]);
    i++;
  }
}

/*
   1 for recyclable, 0 for nonrecyclable
*/
inline int get_recyclability(int category) {
  if (category == -1) {
    return 1;
  } else if (category == -2) {
    return 0;
  } else if (recyclability_set[category] == '1') {
    return 1;
  } else if (recyclability_set[category] == '0') {
    return 0;
  }
}

void print_cat_from_index(LiquidCrystal_I2C lcd, int i_cat) {
  print_line(lcd, category_set[i_cat], false);
//  if (get_recyclability(top_3_cat[i_cat]) == 1) {
//    print_line(lcd, category_set[i_cat], false);
//  }
}

inline boolean capture_send_img() {
  camera_fb_t *fb = NULL;
  esp_err_t res = ESP_OK;
  fb = esp_camera_fb_get();
  if (!fb)
  {
    Serial.println("Camera capture failed");
    print_line(lcd, "Camera capture failed", true);
    esp_camera_fb_return(fb);
    return true;
  }

  size_t fb_len = 0;
  if (fb->format != PIXFORMAT_JPEG)
  {
    Serial.println("Non-JPEG data not implemented");
    print_line(lcd, "Non-JPEG data not implemented", true);
    return true;
  }

  esp_http_client_config_t config = {
    .url = "http://172.20.10.7:8888",
  };

  print_line(lcd, "Uploading image...", true);
  esp_http_client_handle_t client = esp_http_client_init(&config);
  esp_http_client_set_post_field(client, (const char *)fb->buf, fb->len);
  esp_http_client_set_method(client, HTTP_METHOD_POST);
  esp_http_client_set_header(client, "Content-type", "application/octet-stream");

  esp_http_client_open(client, fb->len);
  esp_http_client_write(client, (const char *)fb->buf, fb->len);
  print_line(lcd, "Uploaded, waiting for prediction", true);
  
  esp_http_client_fetch_headers(client);
  int response_length = esp_http_client_get_content_length(client);
  esp_http_client_read(client, response_buffer, response_length);
  esp_err_t err = esp_http_client_close(client);
  Serial.println(response_buffer);
  print_line(lcd, "Prediction obtained, successful", true);
  action = response_buffer[6] - '0';
  for (int i = 0; i < 3; i++) {
    top_3_cat[i] = (response_buffer[i*2]-'0') * 10 + (response_buffer[i*2+1]-'0');
  }
  
  esp_http_client_cleanup(client);
  esp_camera_fb_return(fb);
  
  if (err == ESP_OK) {
    Serial.println("Frame uploaded");
    return false;
  } else {
    Serial.printf("Failed to upload frame, error %d\r\n", err);
    print_line(lcd, "Server error", true);
    return true;
  }
}

/*
 * Dump according to category
 * If input is -1, then dump recyclable
 * If input is -2, then dump nonrecyclable
 * The above cases are handled by get_recyclability()
 * @param category the input category, as explained above
 */
inline void dump(int category) {
  Serial.printf("Dumping %d\n", get_recyclability(category));
  if (get_recyclability(category) == 1) {
    pwm1.setPWM(0, 0, pulseWidth(0));
  } else if (get_recyclability(category) == 0) {
    pwm1.setPWM(0, 0, pulseWidth(90));
  }
}

/*
   Returns category if the human thinks one of the top_3_cat is the true category
   Else, it asks human whether this is recyclable and just return a digit
   representing its recyclability
   @return human input category, -3 if no valid input received
*/
inline int get_human_input(boolean wait_forever) {
  Serial.println("Getting human input");
  int i_cat = 0;
  unsigned long start_time = millis();
  boolean b_prnted = false;
  while ((wait_forever || (millis() - start_time < 3000)) && i_cat <= 3) {
    if (!b_prnted) {
      print_line(lcd, "Is this ", true);
      Serial.println(i_cat);
      Serial.println(top_3_cat[i_cat]);
      if (i_cat != 3) {
        print_cat_from_index(lcd, top_3_cat[i_cat]);
      } else {
        print_line(lcd, "recyclable", false);
      }
      print_line(lcd, "?", false);
      b_prnted = true;
    }
    if (digitalRead(yes_button) == HIGH) {
      if (i_cat == 3) {
        return -1;
      } else {
        return top_3_cat[i_cat];
      }
    } else if (digitalRead(no_button) == HIGH) {
      if (i_cat == 3) {
        return -2;
      } else {
        delay(500);
        i_cat++;
        start_time = millis();
        b_prnted = false;
      }
    }
  }
  return -3;
}

/*
   Send back human input
   If category == -1, then no category info is available, just recyclable,
   if category == -2, then no category info is available, just nonrecyclable,
   if category == -3, then no valid input is received,
   and the image is classified as "others" under recyclable/nonrecyclable category
*/
void send_human_input(int category) {
  // Write the category into human_input_buffer
  if (category >= 0) {
    if (category >= 10) {
      human_input_buffer[0] = '0' + category / 10;
    } else {
      human_input_buffer[0] = '0';
    }
    human_input_buffer[1] = '0' + category % 10;
  } else {
    human_input_buffer[0] = '-';
    int temp = -category;
    human_input_buffer[1] = '0' + temp;
  }
  
  esp_http_client_config_t config = {
    .url = "http://172.20.10.7:8888", // /upload", // replace with my computer's IP address
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);
  esp_http_client_set_post_field(client, (const char *) human_input_buffer, 2);
  esp_http_client_set_method(client, HTTP_METHOD_POST);
  esp_http_client_set_header(client, "Content-type", "text/plain");
  esp_http_client_open(client, 2);
  esp_http_client_write(client, (const char *) human_input_buffer, 2);
  esp_http_client_fetch_headers(client);
  int response_length = esp_http_client_get_content_length(client);
  esp_http_client_read(client, response_buffer, response_length);
  esp_err_t err = esp_http_client_close(client);
  
  if (err == ESP_OK) {
    Serial.println("Category information transmitted");
    print_line(lcd, "Response recorded", true);
  } else {
    Serial.printf("Failed to transmit category information, error %d\r\n", err);
    print_line(lcd, "Error uploading response", true);
  }
  esp_http_client_cleanup(client);
}

void loop() {
  Serial.println("Running");
  pwm1.setPWM(0, 0, pulseWidth(45));
//  state = PROCIMG;
  if (state == READY) {
    // Get sensor input
    // If object is sensed, then go to PROCIMG state
    Serial.println("Ready state");
    if (digitalRead(sensorPin) == LOW) {
      Serial.println("Sensor triggered");
      state = PROCIMG;
      delay(1000); // wait for the object to be fully in place
    }
  } else if (state == PROCIMG) {
    boolean err = capture_send_img();
    if (err == false) {
      Serial.printf("Action: %d\n", action);
      if (action == DUMP) {
        dump(top_3_cat[0]);
      } else if (action == WAIT) {
        // Wait 3s for human input, then dump, then upload input to server
        // If no human input, then follow the prediction
        int curr_cat = get_human_input(false);
        if (curr_cat == -3) { // curr_cat == -3 means no valid input is received
          dump(top_3_cat[0]);
        } else { // Some info is provided by human, so just follow the input
          dump(curr_cat);
        }
        send_human_input(curr_cat);
      } else if (action == WAITFOREVER) {
        // Wait forever for human input, then dump, then upload input to server
        int curr_cat = get_human_input(true);
        dump(curr_cat); // Some info has to be provided by human, so just follow the input
        send_human_input(curr_cat);
      }
      state = READY;
      delay(1000);
    }
  }
}
