#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <driver/rtc_io.h>

// Constants
#define AP_SSID_BASE "GrotBot-" // Base SSID name, will be appended with random number
#define AP_PASSWORD "" // Empty for open network
#define DNS_PORT 53 // Standard DNS port
#define MAX_CONNECTION_ATTEMPTS 10
#define CONNECTION_RETRY_DELAY 1000
#define MAX_FULL_CONNECTION_ATTEMPTS 3  // Number of full connection cycles to try
#define BUTTON_PIN 2 // Button connected to PIN 2
#define SLEEP_TIMEOUT 60000 // 60 seconds timeout before going to sleep

/**
 * About low power mode (USE_LOWER_WIFI_POWER):
 * 
 * Some esp32 c3 super mini are sold with a badly positioned antenna
 * which won't work in higher power mode (more wifi reception)
 * 
 * If the antenna in your chip is "farther" away from the oscilator, you can use higher power
 * mode by setting the USE_LOWER_WIFI_POWER macro to 0
 * 
 * If the antenna is closer, you must use lower power mode by setting the USE_LOWER_WIFI_POWER macro to 1
 * or the wifi will not work at all
 * 
 * You can see this image https://europe1.discourse-cdn.com/arduino/original/4X/1/0/f/10fc721b79ab553c592ee9ee18391cd6125a990d.jpeg
 * (shared from this post https://forum.arduino.cc/t/no-wifi-connect-with-esp32-c3-super-mini/1324046/22)
 * 
 * Here's an excagerated version of the antenna:
 * LEFT (bad)                     RIGHT (good)
 * +------------------------+    +------------------------+
 * |  □ C3             21   |    |  □ C3             21   |
 * |  |                     |    |  |                     |
 * |  |   (short space)     |    |  |  (longer space)     |
 * |  v                     |    |  |                     |
 * |              +-----+   |    |  |                     |
 * |              |     |   |    |  v                     |
 * |              +-----+   |    |              +-----+   |
 * |                        |    |              |     |   |
 * | [    CHIP    ]         |    |              +-----+   |
 * |                        |    |                        |
 * |                        |    | [    CHIP    ]         |
 * 
 * If you are unsure or can't tell the diffrence, keep USE_LOWER_WIFI_POWER to 1
 * but you will have lower wifi reception
 */
#define USE_LOWER_WIFI_POWER 1 // Set to 1 to use lower power (8.5dBm) or 0 to use maximum power (19.5dBm)

// Global variables
WebServer server(80);
DNSServer dnsServer;
Preferences preferences;
String ssid = "";
String password = "";
String webhookUrl = "";
String webhookMethod = "GET";
String webhookHeaders = "";
String webhookPayload = "";
bool configSaved = false;
volatile int pendingRequests = 0;
bool requestInProgress = false;
volatile unsigned long lastButtonPressTime = 0;
unsigned long lastActivityTime = 0; // Track time of last activity
const unsigned long DEBOUNCE_TIME = 300; // Debounce time in milliseconds

// Function prototypes
void setupWiFi();
void setupAP();
void setupWebServer();
String getWiFiStatusString(wl_status_t status);
void handleRoot();
void handleSave();
void sendWebhookRequest();
void goToSleep();
bool isButtonPressed();
void IRAM_ATTR buttonISR();

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\nESP32 C3 Super Mini starting up...");
  Serial.println("Firmware version: 1.0.2 - Auto Sleep");
  
  // Initialize last activity time to current time at boot
  lastActivityTime = millis();

  // Check wake-up reason with detailed debug info
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  Serial.print("Wake up reason code: ");
  Serial.println(wakeup_reason);
  
  // Print human-readable wake-up reason
  switch(wakeup_reason) {
    case ESP_SLEEP_WAKEUP_UNDEFINED:
      Serial.println("Wake up reason: ESP_SLEEP_WAKEUP_UNDEFINED (Normal boot)");
      break;
    case ESP_SLEEP_WAKEUP_GPIO:
      Serial.println("Wake up reason: ESP_SLEEP_WAKEUP_GPIO (Button press)");
      break;
    case ESP_SLEEP_WAKEUP_TIMER:
      Serial.println("Wake up reason: ESP_SLEEP_WAKEUP_TIMER");
      break;
    default:
      Serial.println("Wake up reason: Other reason");
      break;
  }
  
  if (wakeup_reason == ESP_SLEEP_WAKEUP_GPIO) {
    Serial.println("Woken up by button press - will trigger webhook request");
    // Set pending request flag since we were woken by button
    __atomic_fetch_add(&pendingRequests, 1, __ATOMIC_SEQ_CST);
  } else {
    Serial.println("Normal boot or woken by timer");
  }

  // Initialize button pin with internal pull-up resistor
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  Serial.println("Button initialized on PIN 2 with internal pull-up resistor");
  
  // Read the initial state of the button for debugging
  int buttonState = digitalRead(BUTTON_PIN);
  Serial.print("Initial button state: ");
  Serial.println(buttonState == HIGH ? "HIGH (not pressed)" : "LOW (pressed)");
  
  // Attach interrupt to button pin
  attachInterrupt(BUTTON_PIN, buttonISR, FALLING);
  Serial.println("Button interrupt attached");
  
  // Configure GPIO for wakeup - ESP32-C3 specific method
  // For ESP32-C3, only GPIO0-GPIO5 can be used for deep sleep wakeup
  
  // Detach interrupt before configuring deep sleep wake-up to avoid conflicts
  if (wakeup_reason != ESP_SLEEP_WAKEUP_GPIO) {
    // Only detach and reattach if this is not a wake-up from sleep
    // This prevents potential issues with the interrupt handler
    detachInterrupt(BUTTON_PIN);
    delay(100);
    attachInterrupt(BUTTON_PIN, buttonISR, FALLING);
    Serial.println("Interrupt detached and reattached to ensure clean state");
  }
  
  // Enable GPIO wakeup from deep sleep - ESP32-C3 specific method
  // The first parameter is a bitmask of GPIOs that can trigger wakeup
  // The second parameter is the level that triggers wakeup (LOW in our case)
  esp_err_t result = esp_deep_sleep_enable_gpio_wakeup(1ULL << BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
  if (result == ESP_OK) {
    Serial.println("Deep sleep wake-up by button configured successfully on GPIO2");
  } else {
    Serial.print("Failed to configure deep sleep wake-up, error code: ");
    Serial.println(result);
  }

  // Initialize preferences
  preferences.begin("grotbot", false);
  
  // Load saved configuration
  ssid = preferences.getString("ssid", "");
  password = preferences.getString("password", "");
  webhookUrl = preferences.getString("webhook", "");
  webhookMethod = preferences.getString("webhook_method", "GET");
  webhookHeaders = preferences.getString("webhook_headers", "");
  webhookPayload = preferences.getString("webhook_payload", "");
  
  // Trim whitespace from credentials to prevent connection issues
  ssid.trim();
  password.trim();
  webhookUrl.trim();
  
  Serial.println("Loaded configuration (after trimming):");
  Serial.println("SSID: " + ssid);
  Serial.println("SSID length: " + String(ssid.length()));
  Serial.println("Password length: " + String(password.length()));
  Serial.println("Webhook URL: " + webhookUrl);

  // Check if button is currently pressed (force AP mode)
  if (digitalRead(BUTTON_PIN) == LOW) {
    Serial.println("Button is pressed during startup - forcing AP mode");
    setupAP();
    setupWebServer();
  }
  // Otherwise, check if we have saved configuration
  else if (ssid.length() > 0 && password.length() > 0) {
    // Try to connect to WiFi
    setupWiFi();
  } else {
    Serial.println("No saved WiFi credentials, starting AP mode");
    setupAP();
    setupWebServer();
  }
}

void IRAM_ATTR buttonISR() {
  // Simple debounce - ignore button presses that are too close together
  unsigned long currentTime = millis();
  if (currentTime - lastButtonPressTime > DEBOUNCE_TIME) {
    // Use atomic operation to increment the counter to avoid race conditions
    // This ensures the increment operation is not interrupted
    __atomic_fetch_add(&pendingRequests, 1, __ATOMIC_SEQ_CST);
    lastButtonPressTime = currentTime;
    
    // Update activity time in the ISR as well
    // This is safe since we're only writing to it, not reading
    lastActivityTime = currentTime;
    
    Serial.println("Button pressed - Pending requests: " + String(pendingRequests));
  }
}

void loop() {
  wifi_mode_t currentMode = WiFi.getMode();
  
  if (currentMode == WIFI_MODE_AP || (currentMode == WIFI_MODE_STA && WiFi.status() != WL_CONNECTED)) {
    dnsServer.processNextRequest(); // Process DNS requests for captive portal
    server.handleClient();
    
    // If config was just saved, restart to apply new settings
    if (configSaved) {
      Serial.println("Configuration saved, restarting...");
      delay(1000);
      ESP.restart();
    }
    
    // Never go to sleep in AP mode - we need to stay awake for configuration
    
  } else if (currentMode == WIFI_MODE_STA && WiFi.status() == WL_CONNECTED) {
    // Connected to WiFi in station mode
    
    // Check if we have pending requests and no request is currently in progress
    if (pendingRequests > 0 && !requestInProgress) {
      pendingRequests--;
      Serial.println("Processing pending webhook request");
      Serial.println("Pending requests: " + String(pendingRequests));
      sendWebhookRequest(); // Process pending request
      
      // Update activity time since we just processed a request
      lastActivityTime = millis();
    }
    
    // Check if it's time to go to sleep - only in STA mode with connection
    // Only avoid sleep if we're processing a request or have pending requests
    if (!requestInProgress && pendingRequests == 0) {
      if (millis() - lastActivityTime >= SLEEP_TIMEOUT) {
        Serial.println("Sleep timeout reached, going to sleep after " + 
                       String(millis() - lastActivityTime) + "ms of inactivity");
        goToSleep();
      }
    }
  }
  
  // Small delay to avoid excessive CPU usage
  delay(100);
}

void setupWiFi() {
  Serial.println("Connecting to WiFi: " + ssid);
  
  WiFi.mode(WIFI_STA);
  delay(100);
  
  // Try multiple full connection attempts
  for (int fullAttempt = 0; fullAttempt < MAX_FULL_CONNECTION_ATTEMPTS; fullAttempt++) {
    Serial.printf("\nConnection attempt %d of %d\n", fullAttempt + 1, MAX_FULL_CONNECTION_ATTEMPTS);
    
    // Disconnect if we're already trying to connect
    WiFi.disconnect();
    delay(1000);

    // print password and ssid for debug
    Serial.println("SSID:" + ssid);
    
    // Begin connection attempt
    WiFi.begin(ssid.c_str(), password.c_str());
    
    // Set WiFi power based on configuration
    if (USE_LOWER_WIFI_POWER) {
      WiFi.setTxPower(WIFI_POWER_8_5dBm);
    } else {
      WiFi.setTxPower(WIFI_POWER_19_5dBm);
    }
    
    // Try multiple times with reasonable delays
    int attempts = 0;
    
    while (WiFi.status() != WL_CONNECTED && attempts < MAX_CONNECTION_ATTEMPTS) {
      delay(CONNECTION_RETRY_DELAY);
      Serial.print(".");
      
      // Print diagnostic info every few attempts
      if (attempts % 3 == 0) {
        wl_status_t status = WiFi.status();
        Serial.printf("\nAttempt %d/%d - Status: %s\n", 
                     attempts + 1, MAX_CONNECTION_ATTEMPTS, 
                     getWiFiStatusString(status).c_str());
      }
      
      attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nConnected to WiFi");
      Serial.println("IP address: " + WiFi.localIP().toString());
      Serial.printf("Signal strength (RSSI): %d dBm\n", WiFi.RSSI());
      return; // Successfully connected, exit function
    } else {
      wl_status_t status = WiFi.status();
      Serial.printf("\nConnection attempt %d failed. Status: %s\n", 
                   fullAttempt + 1, getWiFiStatusString(status).c_str());
      
      // If this isn't the last attempt, wait before trying again
      if (fullAttempt < MAX_FULL_CONNECTION_ATTEMPTS - 1) {
        Serial.println("Waiting before next connection attempt...");
        delay(3000);
      }
    }
  }
  
  // If we get here, all connection attempts failed
  Serial.println("\nAll connection attempts failed, starting AP mode");
  setupAP();
  setupWebServer();
}



void setupAP() {
  Serial.println("Setting up Access Point with Captive Portal");
  
  // Ensure WiFi is disconnected before switching to AP mode
  WiFi.disconnect(true);
  delay(500);
  
  // Set mode to AP only
  WiFi.mode(WIFI_OFF);
  delay(100);
  WiFi.mode(WIFI_AP);
  delay(500);
  
  // Generate a random number to append to the SSID
  // This helps avoid conflicts with other networks
  String randomizedSSID = AP_SSID_BASE + String(random(1000, 9999));
  
  // Start the AP with the randomized SSID and parameters
  bool apStarted = WiFi.softAP(randomizedSSID.c_str(), AP_PASSWORD, 1, false, 4);
  
  // Set WiFi power based on configuration
  if (USE_LOWER_WIFI_POWER) {
    WiFi.setTxPower(WIFI_POWER_8_5dBm);
  } else {
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
  }
  
  // Debug information
  if (apStarted) {
    Serial.println("AP successfully started!");
  } else {
    Serial.println("Failed to start AP! Check your ESP32 hardware.");
    // Try one more time with default parameters
    delay(1000);
    apStarted = WiFi.softAP(randomizedSSID.c_str(), AP_PASSWORD);
    Serial.println(apStarted ? "Second attempt succeeded!" : "Second attempt also failed!");
  }
  
  // Configure DNS server to redirect all domains to the ESP's IP
  IPAddress apIP = WiFi.softAPIP();
  dnsServer.start(DNS_PORT, "*", apIP);
  
  Serial.println("AP Started with Captive Portal");
  Serial.println("SSID: " + randomizedSSID);
  Serial.println("IP address: " + apIP.toString());
  Serial.println("WiFi mode: " + String(WiFi.getMode()));
  Serial.println("MAC address: " + WiFi.softAPmacAddress());
  Serial.println("Channel: 1 (fixed for better compatibility)");
}

void setupWebServer() {
  // Serve the configuration page for all URLs to create a captive portal effect
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  
  // Handle captive portal detection
  server.on("/generate_204", HTTP_GET, handleRoot);  // Android captive portal detection
  server.on("/connecttest.txt", HTTP_GET, handleRoot); // Microsoft captive portal detection
  server.on("/redirect", HTTP_GET, handleRoot); // Microsoft redirect
  server.on("/hotspot-detect.html", HTTP_GET, handleRoot); // Apple captive portal detection
  server.on("/canonical.html", HTTP_GET, handleRoot); // Apple captive portal detection
  server.on("/success.txt", HTTP_GET, handleRoot); // Apple captive portal detection
  
  // Catch-all handler for any request that doesn't match the ones above
  server.onNotFound([]() {
    server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString(), true);
    server.send(302, "text/plain", ""); // Empty content for redirect
  });
  
  server.begin();
  Serial.println("Web server started with captive portal");
}

void handleRoot() {
  // Update activity time when user accesses the configuration page
  lastActivityTime = millis();
  String html = "<html><head><title>GrotBot Configuration</title>"
                "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                "<style>"
                "body { font-family: Arial, sans-serif; max-width: 800px; margin: 0 auto; padding: 20px; }"
                "h1 { color: #333; text-align: center; }"
                "h2 { color: #444; border-bottom: 1px solid #eee; padding-bottom: 10px; }"
                ".form-group { margin-bottom: 15px; }"
                "label { display: block; margin-bottom: 5px; font-weight: bold; }"
                "input[type='text'], input[type='password'], select, textarea { width: 100%; padding: 8px; box-sizing: border-box; margin-bottom: 10px; }"
                "button { background-color: #4CAF50; color: white; padding: 10px 15px; border: none; cursor: pointer; width: 100%; font-size: 16px; }"
                ".form-section { background: #f9f9f9; padding: 15px; border-radius: 5px; margin-bottom: 20px; }"
                "small { display: block; margin-top: 5px; color: #666; font-size: 0.9em; }"
                "</style></head>"
                "<body><h1>GrotBot Configuration</h1>"
                "<form action='/save' method='post'>"
                
                "<div class='form-section'><h2>WiFi Settings</h2>"
                "<div class='form-group'>"
                "<label for='ssid'>WiFi SSID:</label>"
                "<input type='text' id='ssid' name='ssid' value='" + ssid + "' required>"
                "</div>"
                "<div class='form-group'>"
                "<label for='password'>WiFi Password:</label>"
                "<input type='text' id='password' name='password' value='" + password + "' required>"
                "<small>Make sure there are no extra spaces in your password</small>"
                "</div></div>"
                
                "<div class='form-section'><h2>Webhook Settings</h2>"
                "<div class='form-group'>"
                "<label for='webhook'>Webhook URL:</label>"
                "<input type='text' id='webhook' name='webhook' value='" + webhookUrl + "' required>"
                "</div>"
                
                "<div class='form-group'>"
                "<label for='webhook_method'>HTTP Method:</label>"
                "<select id='webhook_method' name='webhook_method'>"
                "<option value='GET'" + (webhookMethod == "GET" ? " selected" : "") + ">GET</option>"
                "<option value='POST'" + (webhookMethod == "POST" ? " selected" : "") + ">POST</option>"
                "</select>"
                "</div>"
                
                "<div class='form-group'>"
                "<label for='webhook_headers'>Headers (one per line):</label>"
                "<textarea id='webhook_headers' name='webhook_headers' rows='4'>" + webhookHeaders + "</textarea>"
                "<small>Example: Content-Type: application/json</small>"
                "</div>"
                
                "<div class='form-group'>"
                "<label for='webhook_payload'>Request Payload (for POST requests):</label>"
                "<textarea id='webhook_payload' name='webhook_payload' rows='4'>" + webhookPayload + "</textarea>"
                "<small>For JSON, use regular quotes (no escape characters)</small>"
                "</div></div>"
                
                "<button type='submit'>Save and Connect</button>"
                "</form></body></html>";
  
  server.send(200, "text/html", html);
}

void handleSave() {
  // Update activity time when user submits form
  lastActivityTime = millis();
  
  if (server.hasArg("ssid") && server.hasArg("password") && server.hasArg("webhook")) {
    // Get form values and trim whitespace to prevent connection issues
    ssid = server.arg("ssid");
    password = server.arg("password");
    webhookUrl = server.arg("webhook");
    webhookMethod = server.arg("webhook_method");
    webhookHeaders = server.arg("webhook_headers");
    webhookPayload = server.arg("webhook_payload");
    
    // Trim whitespace from beginning and end of credentials
    ssid.trim();
    password.trim();
    webhookUrl.trim();
    webhookMethod.trim();
    webhookHeaders.trim();
    webhookPayload.trim();
    
    Serial.println("Trimmed credentials to remove any extra spaces:");
    Serial.println("SSID length: " + String(ssid.length()));
    Serial.println("Password length: " + String(password.length()));
    
    // Save to preferences
    preferences.putString("ssid", ssid);
    preferences.putString("password", password);
    preferences.putString("webhook", webhookUrl);
    preferences.putString("webhook_method", webhookMethod);
    preferences.putString("webhook_headers", webhookHeaders);
    preferences.putString("webhook_payload", webhookPayload);
    
    Serial.println("New configuration saved:");
    Serial.println("SSID: " + ssid);
    Serial.println("Password: '" + password + "'"); // Print actual password with quotes to see any spaces
    Serial.println("Webhook URL: " + webhookUrl);
    
    String html = "<!DOCTYPE html>"
                  "<html>"
                  "<head>"
                  "<title>Configuration Saved</title>"
                  "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                  "<style>"
                  "body { font-family: Arial, sans-serif; margin: 20px; text-align: center; }"
                  "h1 { color: #4CAF50; }"
                  "</style>"
                  "</head>"
                  "<body>"
                  "<h1>Configuration Saved!</h1>"
                  "<p>The device will now restart and attempt to connect to the WiFi network.</p>"
                  "</body>"
                  "</html>";
    
    server.send(200, "text/html", html);
    configSaved = true;
  } else {
    server.send(400, "text/plain", "Missing required fields");
  }
}

// Helper function to escape JSON strings
String escapeJsonString(const String& input) {
    String output;
    output.reserve(input.length() * 1.1); // Reserve some extra space for escape characters
    
    for (size_t i = 0; i < input.length(); i++) {
        char c = input.charAt(i);
        switch (c) {
            case '\\': output += "\\\\"; break;
            case '\"': output += "\\\""; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (c >= ' ' && c <= '~') {
                    output += c;
                } else {
                    // Convert to \uXXXX for non-printable characters
                    char buf[7];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    output += buf;
                }
        }
    }
    
    return output;
}

void sendWebhookRequest() {
    requestInProgress = true;
    lastActivityTime = millis();

    if (webhookUrl.length() == 0) {
        Serial.println("Webhook URL not set, skipping request");
        requestInProgress = false;
        return;
    }

    HTTPClient http;
    Serial.println("Preparing to send request to: " + webhookUrl);
    // Use WiFiClientSecure for HTTPS and set insecure mode for testing
    WiFiClientSecure client;
    client.setInsecure(); // disables certificate validation (for testing)
    http.begin(client, webhookUrl);

    // Parse headers from free text (one per line, Header: Value)
    int lineStart = 0;
    while (lineStart < webhookHeaders.length()) {
        int lineEnd = webhookHeaders.indexOf('\n', lineStart);
        if (lineEnd == -1) lineEnd = webhookHeaders.length();
        String line = webhookHeaders.substring(lineStart, lineEnd);
        line.trim();
        if (line.length() > 0) {
            int colon = line.indexOf(':');
            if (colon > 0) {
                String key = line.substring(0, colon);
                String value = line.substring(colon + 1);
                key.trim();
                value.trim();
                http.addHeader(key, value);
                Serial.println("Added header: " + key);
            }
        }
        lineStart = lineEnd + 1;
    }

    int httpResponseCode = -1;
    String response;
    if (webhookMethod.equalsIgnoreCase("POST")) {
        // Only send payload for POST
        String payload = webhookPayload;
        Serial.println("Sending POST request with payload: " + payload);
        httpResponseCode = http.POST(payload);
    } else {
        Serial.println("Sending GET request");
        httpResponseCode = http.GET();
    }

    if (httpResponseCode > 0) {
        response = http.getString();
        Serial.println("HTTP Response code: " + String(httpResponseCode));
        Serial.println("Response: " + response);
    } else {
        Serial.println("Error on HTTP request. Error code: " + String(httpResponseCode));
    }

    http.end();
    lastActivityTime = millis();
    requestInProgress = false;
}


void goToSleep() {
  Serial.println("Going to deep sleep. Can be woken by button press only");
  Serial.println("Current button state before sleep: " + String(digitalRead(BUTTON_PIN) == HIGH ? "HIGH (not pressed)" : "LOW (pressed)"));
  
  // Detach interrupt before going to sleep to avoid any potential conflicts
  detachInterrupt(BUTTON_PIN);
  Serial.println("Interrupt detached before sleep");
  
  // Configure GPIO for wake-up again right before sleep
  esp_err_t result = esp_deep_sleep_enable_gpio_wakeup(1ULL << BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
  if (result == ESP_OK) {
    Serial.println("Deep sleep wake-up reconfigured before sleep");
  } else {
    Serial.print("Failed to reconfigure wake-up, error: ");
    Serial.println(result);
  }
  
  Serial.println("Entering deep sleep in 1 second...");
  Serial.flush(); // Make sure all serial data is sent before sleep
  delay(1000);
  esp_deep_sleep_start();
}

bool isButtonPressed() {
  // Since we're using INPUT_PULLUP, the button is pressed when the pin reads LOW
  return digitalRead(BUTTON_PIN) == LOW;
}

// Function to convert WiFi status code to a human-readable string
String getWiFiStatusString(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS:
      return "Idle";
    case WL_NO_SSID_AVAIL:
      return "No SSID Available - Network not found";
    case WL_SCAN_COMPLETED:
      return "Scan Completed";
    case WL_CONNECTED:
      return "Connected";
    case WL_CONNECT_FAILED:
      return "Connection Failed - Wrong password or authentication issue";
    case WL_CONNECTION_LOST:
      return "Connection Lost";
    case WL_DISCONNECTED:
      return "Disconnected - Unable to connect to the network";
    default:
      return "Unknown Status";
  }
}
