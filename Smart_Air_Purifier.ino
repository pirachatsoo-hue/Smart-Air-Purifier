1 | #include <SoftwareSerial.h>
  2 | #include <Wire.h>
  3 | #include <Adafruit_SHT31.h>
  4 | #include <ESP8266WiFi.h>
  5 | #include <PubSubClient.h>
  6 | 
  7 | // =================================================================
  8 | // 1. การกำหนดขาเชื่อมต่ออุปกรณ์ฮาร์ดแวร์ (Hardware Pin Definition)
  9 | // =================================================================
 10 | SoftwareSerial tS(D7, D5);         // สื่อสารแบบ Serial กับหน้าจอ TJC HMI (RX=D7, TX=D5)
 11 | SoftwareSerial dustSerial(D3, D8); // สื่อสารแบบ Serial กับเซนเซอร์ฝุ่น GM1705A01 (RX=D3, TX=D8)
 12 | Adafruit_SHT31 sht31 = Adafruit_SHT31(); // ไลบรารีอ่านค่าอุณหภูมิและความชื้นผ่าน I2C
 13 | 
 14 | const int enA = D4; // ขา PWM สำหรับควบคุมความเร็วพัดลม (L298N Enable A)
 15 | const int in1 = D6; // ขาควบคุมทิศทาง/การหมุนพัดลม (L298N Input 1)
 16 | 
 17 | // =================================================================
 18 | // 2. ตัวแปรเก็บสถานะระบบ (System State Variables)
 19 | // =================================================================
 20 | bool systemPower = true;       // สถานะการเปิด/ปิดเครื่อง (true = ON, false = OFF)
 21 | String currentMode = "AUTO";    // โหมดการทำงานหลักปัจจุบัน ("AUTO", "SLEEP", "TIMER")
 22 | String previousMode = "AUTO";   // ตัวแปรจำโหมดก่อนหน้าสำหรับคืนค่าเมื่อจบ Timer
 23 | int timerHours = 0;             // ตัวแปรกำหนดเวลานับถอยหลัง (0=ปิด, 2, 4, 8 ชั่วโมง)
 24 | unsigned long timerStartMillis = 0; // บันทึกเวลาเริ่มต้นเมื่อกดปุ่มตั้งเวลา
 25 | 
 26 | float pm25_val = 0.0, pm10_val = 0.0; // ตัวแปรเก็บค่าฝุ่น PM2.5 และ PM10 (µg/m³)
 27 | float temp_val = 0.0, hum_val = 0.0;   // ตัวแปรเก็บค่าอุณหภูมิ (°C) และความชื้น (%RH)
 28 | int fanPWM = 0;                        // ค่าความเร็วพัดลมควบคุมด้วยสัญญาณ PWM (0 - 255)
 29 | 
 30 | // =================================================================
 31 | // 3. ตั้งค่าการเชื่อมต่อเครือข่าย WiFi และ MQTT Broker
 32 | // =================================================================
 33 | String MQTT_NAME = "SPU02"; 
 34 | String subTopic = MQTT_NAME + "/command"; // Topic รับคำสั่งจาก Node-RED (SPU02/command)
 35 | String pubTopic = MQTT_NAME + "/status";  // Topic ส่งสถานะไปยัง Node-RED (SPU02/status)
 36 | 
 37 | const char* ssid = "SPU_ERA2.4G";             // ชื่อสัญญาณ WiFi
 38 | const char* password = "sripatum";             // รหัสผ่าน WiFi
 39 | const char* mqtt_server = "plumserver.trueddns.com"; // Domain Server ของ MQTT Broker
 40 | 
 41 | WiFiClient espClient;
 42 | PubSubClient client(espClient);
 43 | 
 44 | unsigned long lastMsg = 0;            // หน่วงเวลาการส่งข้อมูล MQTT
 45 | unsigned long lastDisplayUpdate = 0;  // หน่วงเวลาการอัปเดตหน้าจอ TJC
 46 | 
 47 | // =================================================================
 48 | // 4. ฟังก์ชันการทำงานย่อย (System Helper Functions)
 49 | // =================================================================
 50 | 
 51 | // ฟังก์ชันส่งข้อความแสดงผลไปยังโปรเซสเซอร์หน้าจอ TJC HMI
 52 | void sendToDisplay(String objectName, String textValue) {
 53 |   tS.print(objectName + ".txt=\"" + textValue + "\"");
 54 |   tS.write(0xFF); tS.write(0xFF); tS.write(0xFF); // คำสั่งย้ำจบลูปคำสั่ง TJC
 55 | }
 56 | 
 57 | // ฟังก์ชันสลับเข้าโหมดอัตโนมัติ (AUTO Mode)
 58 | void setAutoMode() {
 59 |   currentMode = "AUTO";
 60 |   previousMode = "AUTO";
 61 |   timerHours = 0;                      // ปลดไทเมอร์เมื่อเลือกเปลี่ยนโหมดการทำงาน
 62 |   sendToDisplay("page0.t8", "OFF");   // รีเซ็ตการแสดงผลเวลาเป็น OFF
 63 |   sendToDisplay("page0.t9", "MODE: " + currentMode);
 64 |   tS.print("dim=100");                // ปรับแสงสว่างหน้าจอขึ้นสูงสุด (100%)
 65 |   tS.write(0xFF); tS.write(0xFF); tS.write(0xFF);
 66 | }
 67 | 
 68 | // ฟังก์ชันสลับเข้าโหมดนอนหลับ (SLEEP Mode)
 69 | void setSleepMode() {
 70 |   currentMode = "SLEEP";
 71 |   previousMode = "SLEEP";
 72 |   timerHours = 0;                      // ปลดไทเมอร์เมื่อเลือกเปลี่ยนโหมดการทำงาน
 73 |   fanPWM = 150;                        // กำหนดพัดลมทำงานความเร็วต่ำเบาเพื่อลดเสียงรบกวน
 74 |   sendToDisplay("page0.t8", "OFF");   // รีเซ็ตการแสดงผลเวลาเป็น OFF
 75 |   sendToDisplay("page0.t9", "MODE: " + currentMode);
 76 |   tS.print("dim=10");                 // หรี่ไฟหน้าจอลงเหลือ 10% เพื่อไม่ให้รบกวนการนอน
 77 |   tS.write(0xFF); tS.write(0xFF); tS.write(0xFF);
 78 | }
 79 | 
 80 | // ฟังก์ชันเปิด/ปิดการทำงานของเครื่องฟอกอากาศ (Power Switch)
 81 | void togglePower() {
 82 |   systemPower = !systemPower;
 83 |   if (!systemPower) {
 84 |     fanPWM = 0;                        // ปิดมอเตอร์พัดลม
 85 |     timerHours = 0;                    // ล้างการตั้งเวลาถอยหลัง
 86 |     currentMode = "OFF";
 87 |     sendToDisplay("page0.t8", "OFF");
 88 |     sendToDisplay("page0.t9", "CLOSE"); // แสดงข้อความ CLOSE เมื่อปิดเครื่อง
 89 |     tS.print("dim=5");                 // หรี่ไฟหน้าจอลงเหลือ 5% ให้ทราบว่ามีไฟเลี้ยงอยู่
 90 |     tS.write(0xFF); tS.write(0xFF); tS.write(0xFF);
 91 |   } else {
 92 |     setAutoMode();                    // เริ่มต้นเปิดเครื่องกลับเข้าสู่โหมด AUTO
 93 |   }
 94 | }
 95 | 
 96 | // ฟังก์ชันประมวลผลคำสั่งระบบตั้งเวลาถอยหลัง (Timer Subroutine)
 97 | void handleTimerCommand() {
 98 |   if (timerHours == 0) timerHours = 2;
 99 |   else if (timerHours == 2) timerHours = 4;
100 |   else if (timerHours == 4) timerHours = 8;
101 |   else timerHours = 0;
102 |   
103 |   timerStartMillis = millis(); // บันทึกเวลาประมวลผลเริ่มต้น
104 | 
105 |   if (timerHours > 0) {
106 |     fanPWM = 255;                       // เร่งความเร็วพัดลมเต็มกำลังเมื่อเข้าสู่โหมดตั้งเวลา
107 |     currentMode = "TIMER";              // ปรับเปลี่ยนโหมดสถานะเป็น TIMER
108 |     sendToDisplay("page0.t8", String(timerHours) + "H");
109 |     sendToDisplay("page0.t9", "TIMER " + String(timerHours) + "H");
110 |   } else {
111 |     currentMode = previousMode;         // สลับคืนค่ากลับเข้าสู่โหมดหลักก่อนหน้า
112 |     sendToDisplay("page0.t8", "OFF");
113 |     sendToDisplay("page0.t9", "MODE: " + currentMode);
114 |   }
115 | }
116 | 
117 | // =================================================================
118 | // 5. ฟังก์ชันรับข้อมูลคำสั่งจาก MQTT Broker (Callback Node-RED)
119 | // =================================================================
120 | void callback(char* topic, byte* payload, unsigned int length) {
121 |   String msg = "";
122 |   for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
123 |   msg.trim();
124 | 
125 |   if (msg == "CMD_POWER") {
126 |     togglePower();
127 |   }
128 |   else if (msg == "CMD_TIMER") {
129 |     if (!systemPower) {
130 |       systemPower = true;               // หากเครื่องปิดอยู่ ปรับเปิดเครื่องเพื่อตั้งเวลาทันที
131 |       tS.print("dim=100");
132 |       tS.write(0xFF); tS.write(0xFF); tS.write(0xFF);
133 |     }
134 |     handleTimerCommand();
135 |   }
136 |   else if (systemPower) {               // ประมวลผลคำสั่งอื่นๆ เมื่อเครื่องอยู่ในสถานะ ON
137 |     if (msg == "FAN_ON" && (currentMode == "AUTO" || currentMode == "TIMER")) {
138 |       fanPWM = 255;
139 |     } 
140 |     else if (msg == "FAN_OFF" && (currentMode == "AUTO" || currentMode == "TIMER")) {
141 |       fanPWM = 0;
142 |     }
143 |     else if (msg == "CMD_AUTO") setAutoMode();
144 |     else if (msg == "CMD_SLEEP") setSleepMode();
145 |   }
146 | }
147 | 
148 | // =================================================================
149 | // 6. การตั้งค่าเริ่มต้นระบบ (Setup Routine)
150 | // =================================================================
151 | void setup() {
152 |   Serial.begin(115200);
153 |   tS.begin(9600);
154 |   dustSerial.begin(9600);

155 |   pinMode(enA, OUTPUT);
156 |   pinMode(in1, OUTPUT);

157 |   Wire.begin(D2, D1); 
158 |   sht31.begin(0x44);

159 |   // ทดสอบการทำงานของพัดลมชั่วคราวตอนเริ่มต้นระบบ (Self-check Test)
160 |   digitalWrite(in1, HIGH);
161 |   analogWrite(enA, 200);
162 |   delay(1000);
163 |   analogWrite(enA, 0);

164 |   // เชื่อมต่อสัญญาณ WiFi และ MQTT Server
165 |   WiFi.mode(WIFI_STA);
166 |   WiFi.begin(ssid, password);
167 |   client.setServer(mqtt_server, 34733);
168 |   client.setCallback(callback);
169 | }

170 | // =================================================================
171 | // 7. ลูปการทำงานหลักต่อเนื่อง (Main Loop Routine)
172 | // =================================================================
173 | void loop() {
174 |   // ตรวจสอบการเชื่อมต่อ WiFi/MQTT และ Reconnect อัตโนมัติเมื่อหลุด
175 |   if (WiFi.status() == WL_CONNECTED) {
176 |     if (!client.connected()) {
177 |       if (client.connect(MQTT_NAME.c_str(), "spu", "sripatum")) {
178 |         client.subscribe(subTopic.c_str());
179 |       }
180 |     }
181 |     client.loop();
182 |   }

183 |   // --- Section 7.1: รับคำสั่งกดปุ่มอินพุตจากจอ TJC HMI ---
184 |   tS.listen();
185 |   while (tS.available() > 0) {
186 |     String cmd = tS.readStringUntil('\n');
187 |     cmd.trim();
188 |     
189 |     if (cmd == "CMD_POWER") togglePower();
190 |     
191 |     if (systemPower) {
192 |       if (cmd == "CMD_AUTO") setAutoMode();
193 |       else if (cmd == "CMD_SLEEP") setSleepMode();
194 |       else if (cmd == "CMD_TIMER") {
195 |         handleTimerCommand();
196 |       }
197 |     }
198 |   }

199 |   // --- Section 7.2: ประมวลผลเวลานับถอยหลัง (Timer Countdown Check) ---
200 |   if (systemPower && timerHours > 0) {
201 |     unsigned long timerDuration = (unsigned long)timerHours * 3600000;
202 |     if (millis() - timerStartMillis >= timerDuration) {
203 |       systemPower = false;              // หมดเวลาที่กำหนด สั่งปิดระบบทันที
204 |       fanPWM = 0;
205 |       timerHours = 0;
206 |       currentMode = "OFF";
207 |       sendToDisplay("page0.t8", "OFF");
208 |       sendToDisplay("page0.t9", "CLOSE");
209 |       tS.print("dim=5");
210 |       tS.write(0xFF); tS.write(0xFF); tS.write(0xFF);
211 |     }
212 |   }

213 |   // --- Section 7.3: อ่านค่าฝุ่นจากเซนเซอร์ GM1705A01 ---
214 |   dustSerial.listen();
215 |   while (dustSerial.available() >= 32) {
216 |     if (dustSerial.read() == 0x42 && dustSerial.read() == 0x4D) {
217 |       uint8_t buffer[30];
218 |       if (dustSerial.readBytes(buffer, 30) == 30) {
219 |         pm25_val = (buffer[4] << 8) | buffer[5];
220 |         pm10_val = (buffer[6] << 8) | buffer[7];
221 |       }
222 |       break;
223 |     }
224 |   }

225 |   // --- Section 7.4: อ่านค่าอุณหภูมิและความชื้นจาก SHT31 ---
226 |   float t_read = sht31.readTemperature();
227 |   float h_read = sht31.readHumidity();
228 |   if (!isnan(t_read)) temp_val = t_read;
229 |   if (!isnan(h_read)) hum_val = h_read;

230 |   // --- Section 7.5: ควบคุมมอเตอร์พัดลมตามสภาวะระบบ ---
231 |   if (!systemPower) {
232 |     analogWrite(enA, 0);
233 |     digitalWrite(in1, LOW);
234 |   } else {
235 |     digitalWrite(in1, HIGH);
236 |     analogWrite(enA, fanPWM);
237 |   }

238 |   // --- Section 7.6: ส่งค่าอัปเดตสถานะขึ้นจอ TJC HMI ทุกๆ 500ms ---
239 |   unsigned long now = millis();
240 |   if (now - lastDisplayUpdate >= 500) {
241 |     lastDisplayUpdate = now;
242 |     if (systemPower) {
243 |       sendToDisplay("page0.t0", String(pm25_val, 0));
244 |       sendToDisplay("page0.t1", String(pm10_val, 0));
245 |       sendToDisplay("page0.t2", String(temp_val, 1));
246 |       sendToDisplay("page0.t3", String(hum_val, 1));
247 |       sendToDisplay("page0.t8", (timerHours > 0) ? String(timerHours) + "H" : "OFF");
248 |       
249 |       if (timerHours == 0) {
250 |         sendToDisplay("page0.t9", "MODE: " + currentMode);
251 |       }
252 |     }
253 |   }

254 |   // --- Section 7.7: พับลิชค่าสถานะ CSV Payload สู่ Node-RED ผ่าน MQTT ---
255 |   if (WiFi.status() == WL_CONNECTED && client.connected()) {
256 |     if (now - lastMsg > 1000) {
257 |       lastMsg = now;

258 |       String modeToSend = currentMode;
259 |       if (timerHours > 0) {
260 |         modeToSend = "TIMER";
261 |       }

262 |       String payload = String(pm25_val, 0) + "," +
263 |                        String(pm10_val, 0) + "," +
264 |                        String(temp_val, 1) + "," +
265 |                        String(hum_val, 1) + "," +
266 |                        (systemPower ? "ON" : "OFF") + "," +
267 |                        modeToSend + "," +
268 |                        String(timerHours);

269 |       client.publish(pubTopic.c_str(), payload.c_str());
270 |     }
271 |   }
272 | }