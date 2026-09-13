// ESP32-C3 离线设备信息交换固件（Arduino-ESP32）
#include <ArduinoJson.h>
#include <BLEDevice.h>
#include <BLEAdvertising.h>
#include <BLEScan.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_now.h>
#include <string>

constexpr char MQTT_HOST[] = "broker.emqx.io";
constexpr uint16_t MQTT_PORT = 1883;
constexpr uint32_t BLE_SCAN_INTERVAL_MS = 60000;

constexpr uint8_t MAX_NEARBY_DEVICES = 50;
constexpr uint8_t MAX_SAVED_NEARBY_DEVICES = 10;
constexpr uint32_t NEARBY_DEVICE_TIMEOUT_MS = 120000;
constexpr uint32_t ESP_NOW_INTERVAL_MS = 10000;
constexpr uint8_t ESP_NOW_CHANNEL = 6;
constexpr uint8_t ESP_NOW_MAGIC = 0xD3;

Preferences preferences;
WebServer server(80);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
BLEScan* bleScan = nullptr;
BLEAdvertising* bleAdvertising = nullptr;

String deviceId;
String name;
String nickname;
String bio;
String note;
String statusText;
String qq;
String wx;
String wifiSsid;
String wifiPassword;
String mqttRoot;
uint32_t lastMqttAttempt = 0;
uint32_t lastBleScan = 0;
uint32_t lastEspNowBroadcast = 0;
volatile bool espNowReady = false;
volatile bool espNowMessagePending = false;

struct EspNowProfile {
	uint8_t magic;
	uint8_t version;
	char deviceId[32];
	char name[48];
	char nickname[48];
	char bio[128];
	char status[32];
	char note[96];
	char qq[20];
	char wx[32];
};

EspNowProfile pendingEspNowProfile;
uint8_t espNowBroadcastAddress[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

struct NearbyDevice {
	String deviceId;
	String name;
	String nickname;
	String bio;
	String status;
	String note;
	String qq;
	String wx;
	uint32_t lastSeen;
};

NearbyDevice nearbyDevices[MAX_NEARBY_DEVICES];

String nearbyKey(uint8_t index, const char* field) {
	return "n" + String(index) + field;
}

void saveNearbyDevice(uint8_t index) {
	if (index >= MAX_SAVED_NEARBY_DEVICES || nearbyDevices[index].deviceId.isEmpty()) return;
	NearbyDevice& nearby = nearbyDevices[index];
	preferences.putString(nearbyKey(index, "id").c_str(), nearby.deviceId);
	preferences.putString(nearbyKey(index, "name").c_str(), nearby.name);
	preferences.putString(nearbyKey(index, "nick").c_str(), nearby.nickname);
	preferences.putString(nearbyKey(index, "bio").c_str(), nearby.bio);
	preferences.putString(nearbyKey(index, "stat").c_str(), nearby.status);
	preferences.putString(nearbyKey(index, "note").c_str(), nearby.note);
	preferences.putString(nearbyKey(index, "qq").c_str(), nearby.qq);
	preferences.putString(nearbyKey(index, "wx").c_str(), nearby.wx);
}

void loadNearbyDevices() {
	for (uint8_t index = 0; index < MAX_SAVED_NEARBY_DEVICES; index++) {
		NearbyDevice& nearby = nearbyDevices[index];
		nearby.deviceId = preferences.getString(nearbyKey(index, "id").c_str(), "");
		if (nearby.deviceId.isEmpty()) continue;
		nearby.name = preferences.getString(nearbyKey(index, "name").c_str(), "");
		nearby.nickname = preferences.getString(nearbyKey(index, "nick").c_str(), "");
		nearby.bio = preferences.getString(nearbyKey(index, "bio").c_str(), "");
		nearby.status = preferences.getString(nearbyKey(index, "stat").c_str(), "");
		nearby.note = preferences.getString(nearbyKey(index, "note").c_str(), "");
		nearby.qq = preferences.getString(nearbyKey(index, "qq").c_str(), "");
		nearby.wx = preferences.getString(nearbyKey(index, "wx").c_str(), "");
		nearby.lastSeen = millis();
	}
}

String jsonEscape(const String& value) {
	String escaped = value;
	escaped.replace("\\", "\\\\");
	escaped.replace("\"", "\\\"");
	escaped.replace("\n", "\\n");
	return escaped;
}

String htmlEscape(const String& value) {
	String escaped = value;
	escaped.replace("&", "&amp;");
	escaped.replace("<", "&lt;");
	escaped.replace(">", "&gt;");
	escaped.replace("\"", "&quot;");
	escaped.replace("'", "&#39;");
	return escaped;
}

String configInput(const String& label, const String& field, const String& value, const String& type = "text") {
	return "<form method='post' action='/api/config'><label>" + label + "</label><input name='" + field +
		"' type='" + type + "' value='" + htmlEscape(value) + "'><button type='submit'>保存</button></form>";
}

String configTextarea(const String& label, const String& field, const String& value) {
	return "<form method='post' action='/api/config'><label>" + label + "</label><textarea name='" + field +
		"'>" + htmlEscape(value) + "</textarea><button type='submit'>保存</button></form>";
}

void loadSettings() {
	preferences.begin("device", false);
	if (!preferences.getBool("wifiResetDone", false)) {
		preferences.remove("ssid");
		preferences.remove("password");
		preferences.putBool("wifiResetDone", true);
	}
	wifiSsid = preferences.getString("ssid", "");
	wifiPassword = preferences.getString("password", "");
	name = preferences.getString("name", "未命名设备");
	nickname = preferences.getString("nickname", "未命名设备");
	bio = preferences.getString("bio", "");
	note = preferences.getString("note", "");
	statusText = preferences.getString("status", "远征");
	qq = preferences.getString("qq", "1811610638");
	wx = preferences.getString("wx", "_eternall");
	mqttRoot = preferences.getString("mqttRoot", "devices");
	uint32_t chipId = static_cast<uint32_t>(ESP.getEfuseMac() & 0xFFFFFF);
	deviceId = "c3-" + String(chipId, HEX);
	deviceId.toUpperCase();
	statusText = "远征";
	qq = "1811610638";
	wx = "_eternall";
	preferences.putString("deviceId", deviceId);
	preferences.putString("status", statusText);
	preferences.putString("qq", qq);
	preferences.putString("wx", wx);
	loadNearbyDevices();
}

void saveProfile() {
	preferences.putString("name", name);
	preferences.putString("nickname", nickname);
	preferences.putString("bio", bio);
	preferences.putString("note", note);
	preferences.putString("status", statusText);
	preferences.putString("qq", qq);
	preferences.putString("wx", wx);
}

void sendJson(int code, const String& body) {
	server.sendHeader("Access-Control-Allow-Origin", "*");
	server.send(code, "application/json; charset=utf-8", body);
}

String profileJson(bool includeNearby = false) {
	String body = "{\"deviceId\":\"" + jsonEscape(deviceId) + "\",\"name\":\"" + jsonEscape(name) +
		"\",\"nickname\":\"" + jsonEscape(nickname) + "\",\"bio\":\"" + jsonEscape(bio) +
		"\",\"note\":\"" + jsonEscape(note) + "\",\"status\":\"" + jsonEscape(statusText) +
		"\",\"qq\":\"" + jsonEscape(qq) + "\",\"wx\":\"" + jsonEscape(wx) +
		"\",\"online\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false");
	if (includeNearby) {
		body += ",\"nearby\":[";
		bool firstDevice = true;
		for (const NearbyDevice& nearby : nearbyDevices) {
			if (nearby.deviceId.isEmpty()) continue;
			if (!firstDevice) body += ',';
			firstDevice = false;
			body += "{\"deviceId\":\"" + jsonEscape(nearby.deviceId) + "\",\"name\":\"" +
				jsonEscape(nearby.name) + "\",\"nickname\":\"" + jsonEscape(nearby.nickname) +
				"\",\"bio\":\"" + jsonEscape(nearby.bio) + "\",\"note\":\"" + jsonEscape(nearby.note) +
				"\",\"status\":\"" + jsonEscape(nearby.status) + "\",\"qq\":\"" +
				jsonEscape(nearby.qq) + "\",\"wx\":\"" + jsonEscape(nearby.wx) + "\"}";
		}
		body += "]";
	}
	return body + "}";
}

void handleRoot() {
	cleanupNearbyDevices();
	String nearbyHtml = "<section><h3>附近设备状态</h3>";
	bool hasNearbyDevice = false;
	for (const NearbyDevice& nearby : nearbyDevices) {
		if (nearby.deviceId.isEmpty()) continue;
		hasNearbyDevice = true;
		nearbyHtml += "<p><strong>" + htmlEscape(nearby.name.isEmpty() ? (nearby.nickname.isEmpty() ? nearby.deviceId : nearby.nickname) : nearby.name) +
			"</strong><br>设备 ID：" + htmlEscape(nearby.deviceId) +
			"<br>状态：<span>" + htmlEscape(nearby.status) + "</span></p>";
	}
	if (!hasNearbyDevice) nearbyHtml += "<p>暂未发现附近设备</p>";
	nearbyHtml += "<p><a href='/api/devices'>查看交换数据</a></p></section>";
	String page = "<!doctype html><meta name='viewport' content='width=device-width'><title>设备信息</title>"
		"<style>body{font:16px sans-serif;max-width:520px;margin:30px auto;padding:0 16px}"
		"section{border:1px solid #ddd;padding:16px;margin-bottom:20px}dt{font-weight:bold;margin-top:8px}"
		"input,textarea{box-sizing:border-box;width:100%;padding:10px;margin:5px 0 12px}button{padding:10px 18px}</style>"
		"<h2>设备信息</h2><section><dl>"
		"<dt>设备 ID</dt><dd>" + htmlEscape(deviceId) + "</dd>"
		"<dt>名称</dt><dd>" + htmlEscape(name) + "</dd>"
		"<dt>个人简介</dt><dd>" + htmlEscape(bio) + "</dd>"
		"<dt>状态</dt><dd>" + htmlEscape(statusText) + "</dd>"
		"<dt>QQ</dt><dd>" + htmlEscape(qq) + "</dd>"
		"<dt>微信</dt><dd>" + htmlEscape(wx) + "</dd>"
		"</dl></section>" + nearbyHtml +
		"<h2>设备配置</h2>" + configInput("WiFi 名称", "ssid", wifiSsid) +
		configInput("WiFi 密码", "password", "", "password") + configInput("名称", "name", name) +
		configInput("昵称", "nickname", nickname) + configTextarea("个人简介", "bio", bio) +
		configTextarea("备注", "note", note) +
		configInput("自定义状态", "status", statusText) + configInput("QQ", "qq", qq) +
		configInput("微信", "wx", wx) +
		"<form method='post' action='/api/reset-wlan' onsubmit=\"return confirm('确定清除 WLAN 配置？')\">"
		"<button type='submit'>重置 WLAN 配置</button></form>";
	server.send(200, "text/html; charset=utf-8", page);
}

void handleConfig() {
	if (server.hasArg("ssid")) preferences.putString("ssid", server.arg("ssid"));
	if (server.hasArg("password")) preferences.putString("password", server.arg("password"));
	if (server.hasArg("name")) name = server.arg("name");
	if (server.hasArg("nickname")) nickname = server.arg("nickname");
	if (server.hasArg("bio")) bio = server.arg("bio");
	if (server.hasArg("note")) note = server.arg("note");
	if (server.hasArg("status")) statusText = server.arg("status");
	if (server.hasArg("qq")) qq = server.arg("qq");
	if (server.hasArg("wx")) wx = server.arg("wx");
	saveProfile();
	server.send(200, "text/plain; charset=utf-8", "已保存，设备将在 2 秒后重启喵");
	delay(2000);
	ESP.restart();
}

void handleWifiReset() {
	preferences.remove("ssid");
	preferences.remove("password");
	server.send(200, "text/plain; charset=utf-8", "WLAN 配置已清除，设备将在 2 秒后重启");
	delay(2000);
	ESP.restart();
}

void handleProfile() {
	if (server.method() == HTTP_POST) {
		DynamicJsonDocument document(768);
		if (deserializeJson(document, server.arg("plain"))) {
			sendJson(400, "{\"error\":\"invalid json\"}");
			return;
		}
		if (document.containsKey("name")) name = document["name"].as<String>();
		if (document.containsKey("nickname")) nickname = document["nickname"].as<String>();
		if (document.containsKey("bio")) bio = document["bio"].as<String>();
		if (document.containsKey("note")) note = document["note"].as<String>();
		if (document.containsKey("status")) statusText = document["status"].as<String>();
		if (document.containsKey("qq")) qq = document["qq"].as<String>();
		if (document.containsKey("wx")) wx = document["wx"].as<String>();
		saveProfile();
	}
	sendJson(200, profileJson());
}

void cleanupNearbyDevices() {
	// Nearby profiles are persistent and remain visible until replaced.
}

void updateNearbyDevice(const String& foundId, const String& foundName, const String& foundNickname, const String& foundBio, const String& foundStatus,
	const String& foundNote = "", const String& foundQq = "", const String& foundWx = "") {
	if (foundId.isEmpty() || foundId == deviceId) return;
	cleanupNearbyDevices();
	NearbyDevice* freeSlot = nullptr;
	NearbyDevice* oldest = &nearbyDevices[0];
	for (NearbyDevice& nearby : nearbyDevices) {
		if (nearby.deviceId == foundId) {
			uint8_t index = static_cast<uint8_t>(&nearby - nearbyDevices);
			nearby.name = foundName;
			nearby.nickname = foundNickname;
			nearby.bio = foundBio;
			nearby.status = foundStatus;
			nearby.note = foundNote;
			nearby.qq = foundQq;
			nearby.wx = foundWx;
			nearby.lastSeen = millis();
			saveNearbyDevice(index);
			return;
		}
		if (nearby.deviceId.isEmpty() && freeSlot == nullptr) freeSlot = &nearby;
		if (nearby.lastSeen < oldest->lastSeen) oldest = &nearby;
	}
	NearbyDevice& target = freeSlot != nullptr ? *freeSlot : *oldest;
	target.deviceId = foundId;
	target.name = foundName;
	target.nickname = foundNickname;
	target.bio = foundBio;
	target.status = foundStatus;
	target.note = foundNote;
	target.qq = foundQq;
	target.wx = foundWx;
	target.lastSeen = millis();
	saveNearbyDevice(static_cast<uint8_t>(&target - nearbyDevices));
}

void copyToEspNowField(char* destination, size_t size, const String& value) {
	value.toCharArray(destination, size);
}

EspNowProfile localEspNowProfile() {
	EspNowProfile profile{};
	profile.magic = ESP_NOW_MAGIC;
	profile.version = 3;
	copyToEspNowField(profile.deviceId, sizeof(profile.deviceId), deviceId);
	copyToEspNowField(profile.name, sizeof(profile.name), name);
	copyToEspNowField(profile.nickname, sizeof(profile.nickname), nickname);
	copyToEspNowField(profile.bio, sizeof(profile.bio), bio);
	copyToEspNowField(profile.status, sizeof(profile.status), statusText);
	copyToEspNowField(profile.note, sizeof(profile.note), note);
	copyToEspNowField(profile.qq, sizeof(profile.qq), qq);
	copyToEspNowField(profile.wx, sizeof(profile.wx), wx);
	return profile;
}

void onEspNowReceive(const uint8_t*, const uint8_t* data, int length) {
	if (length != sizeof(EspNowProfile) || espNowMessagePending) return;
	EspNowProfile received{};
	memcpy(&received, data, sizeof(received));
	if (received.magic != ESP_NOW_MAGIC || received.version != 1 || received.deviceId[0] == '\0') return;
	pendingEspNowProfile = received;
	espNowMessagePending = true;
}

void setupEspNow() {
	if (esp_now_init() != ESP_OK) {
		Serial.println("ESP-NOW 初始化失败");
		return;
	}
	esp_now_peer_info_t peer{};
	memcpy(peer.peer_addr, espNowBroadcastAddress, sizeof(espNowBroadcastAddress));
	peer.channel = 0;
	peer.encrypt = false;
	if (esp_now_add_peer(&peer) != ESP_OK && !esp_now_is_peer_exist(espNowBroadcastAddress)) {
		Serial.println("ESP-NOW 广播节点添加失败");
		return;
	}
	esp_now_register_recv_cb(onEspNowReceive);
	espNowReady = true;
	Serial.println("ESP-NOW 离线数据交换已启动");
}

void broadcastEspNowProfile() {
	if (!espNowReady) return;
	EspNowProfile profile = localEspNowProfile();
	esp_err_t result = esp_now_send(espNowBroadcastAddress, reinterpret_cast<const uint8_t*>(&profile), sizeof(profile));
	if (result != ESP_OK) Serial.printf("ESP-NOW 发送失败: %d\n", result);
}

void processEspNowProfile() {
	if (!espNowMessagePending) return;
	EspNowProfile received = pendingEspNowProfile;
	espNowMessagePending = false;
	String foundId = received.deviceId;
	if (foundId == deviceId) return;
	updateNearbyDevice(foundId, received.name, received.nickname, received.bio, received.status, received.note, received.qq, received.wx);
	Serial.printf("离线交换设备资料: %s (%s)\n", foundId.c_str(), received.status);
}

void publishProfile() {
	if (!mqtt.connected()) return;
	String topic = mqttRoot + "/" + deviceId + "/profile";
	mqtt.publish(topic.c_str(), profileJson().c_str(), true);
}

void onOtherDeviceProfile(const String& topic, const String& payload) {
	Serial.printf("收到设备资料 %s: %s\n", topic.c_str(), payload.c_str());
}

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
	DynamicJsonDocument document(768);
	String input;
	for (unsigned int index = 0; index < length; index++) input += static_cast<char>(payload[index]);
	String topicText = topic;
	if (topicText.endsWith("/profile") && !topicText.endsWith(("/" + deviceId + "/profile").c_str())) {
		onOtherDeviceProfile(topicText, input);
		return;
	}
	if (deserializeJson(document, input)) return;
	if (document.containsKey("name")) name = document["name"].as<String>();
	if (document.containsKey("nickname")) nickname = document["nickname"].as<String>();
	if (document.containsKey("bio")) bio = document["bio"].as<String>();
	if (document.containsKey("note")) note = document["note"].as<String>();
	if (document.containsKey("status")) statusText = document["status"].as<String>();
	saveProfile();
	publishProfile();
}

void connectMqtt() {
	if (WiFi.status() != WL_CONNECTED || mqtt.connected() || millis() - lastMqttAttempt < 5000) return;
	lastMqttAttempt = millis();
	String clientId = "esp32-" + deviceId;
	if (mqtt.connect(clientId.c_str())) {
		String commandTopic = mqttRoot + "/" + deviceId + "/set";
		String broadcastTopic = mqttRoot + "/broadcast/set";
		String allProfilesTopic = mqttRoot + "/+/profile";
		mqtt.subscribe(commandTopic.c_str());
		mqtt.subscribe(broadcastTopic.c_str());
		mqtt.subscribe(allProfilesTopic.c_str());
		publishProfile();
		Serial.println("MQTT 已连接并订阅设备与广播主题");
	}
}

void startBleBroadcast() {
	BLEAdvertisementData advertisement;
	String identity = "P|" + deviceId + "|" + statusText;
	if (identity.length() > 24) identity = "P|" + deviceId;
	advertisement.setName(("D-" + deviceId.substring(deviceId.length() - 6)).c_str());
	advertisement.setManufacturerData(std::string(identity.c_str()));
	bleAdvertising = BLEDevice::getAdvertising();
	bleAdvertising->setAdvertisementData(advertisement);
	bleAdvertising->start();
	Serial.println("BLE 广播已启动");
}

void scanBle() {
	if (!bleScan) return;
	BLEScanResults results = bleScan->start(4, false);
	for (int index = 0; index < results.getCount(); index++) {
		BLEAdvertisedDevice device = results.getDevice(index);
		if (!device.haveManufacturerData()) continue;
		String data = device.getManufacturerData().c_str();
		if (!data.startsWith("P|")) continue;
		int separator = data.indexOf('|', 2);
		if (separator <= 2) continue;
		String foundId = data.substring(2, separator);
		if (foundId == deviceId) continue;
		String foundNickname = device.haveName() ? device.getName().c_str() : "";
		String foundStatus = data.substring(separator + 1);
		updateNearbyDevice(foundId, "", foundNickname, "", foundStatus);
		Serial.printf("发现附近设备 %s (%s)\n", foundId.c_str(), foundStatus.c_str());
	}
	bleScan->clearResults();
}

void startConfigAccessPoint() {
	String apName = "ESP32-" + deviceId.substring(deviceId.length() - 6);
	IPAddress apIp(192, 168, 4, 1);
	IPAddress gateway(192, 168, 4, 1);
	IPAddress subnet(255, 255, 255, 0);
	WiFi.mode(WIFI_AP);
	WiFi.setSleep(false);
	WiFi.softAPConfig(apIp, gateway, subnet);
	bool started = WiFi.softAP(apName.c_str(), "12345678", ESP_NOW_CHANNEL, false, 4);
	Serial.printf("配置热点 %s，信道 %u，启动%s，地址 http://%s/\n", apName.c_str(), ESP_NOW_CHANNEL,
		started ? "成功" : "失败", WiFi.softAPIP().toString().c_str());
}

void connectWiFi() {
	startConfigAccessPoint();
}

void setup() {
	Serial.begin(115200);
	loadSettings();
	connectWiFi();
	server.on("/", HTTP_GET, handleRoot);
	server.on("/api/profile", HTTP_ANY, handleProfile);
	server.on("/api/reset-wlan", HTTP_POST, handleWifiReset);
	server.on("/api/devices", HTTP_GET, []() {
		cleanupNearbyDevices();
		sendJson(200, profileJson(true));
	});
	server.on("/api/config", HTTP_POST, handleConfig);
	server.begin();
	mqtt.setServer(MQTT_HOST, MQTT_PORT);
	mqtt.setCallback(onMqttMessage);
	BLEDevice::init("");
	bleScan = BLEDevice::getScan();
	bleScan->setActiveScan(true);
	bleScan->setInterval(100);
	bleScan->setWindow(80);
	setupEspNow();
	startBleBroadcast();
	Serial.println("设备服务已启动，ID: " + deviceId);
}

void loop() {
	server.handleClient();
	processEspNowProfile();
	connectMqtt();
	mqtt.loop();
	if (millis() - lastEspNowBroadcast >= ESP_NOW_INTERVAL_MS) {
		lastEspNowBroadcast = millis();
		broadcastEspNowProfile();
	}
	if (millis() - lastBleScan >= BLE_SCAN_INTERVAL_MS) {
		lastBleScan = millis();
		scanBle();
	}
	static uint32_t lastHeartbeat = 0;
	if (mqtt.connected() && millis() - lastHeartbeat >= 60000) {
		lastHeartbeat = millis();
		publishProfile();
	}
}
