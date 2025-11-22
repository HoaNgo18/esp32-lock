
const MQTT_SERVER = "e8d92a81786b4564b07b2759213912d0.s1.eu.hivemq.cloud";
const MQTT_PORT   = 8884;
const MQTT_PATH   = "/mqtt";  
const MQTT_USER   = "esp32_user";
const MQTT_PASS   = "Password123";

//Topic
const MQTT_CMD_TOPIC = "lock/cmd";
const MQTT_LOG_TOPIC = "lock/log";

// DOM
const statusDiv = document.getElementById("status");
const logList   = document.getElementById("log-list");
const btnAdd    = document.getElementById("btn-add");
const btnDel    = document.getElementById("btn-del");

const MQTT_CLIENT_ID = "web_admin_" + Math.floor(Math.random() * 100000);

//Khoi tao Paho MQTT
var client = new Paho.MQTT.Client(
  MQTT_SERVER,
  Number(MQTT_PORT),
  MQTT_PATH,
  MQTT_CLIENT_ID
);

client.onConnectionLost = onConnectionLost;
client.onMessageArrived = onMessageArrived;

var connectOptions = {
  timeout: 10,
  useSSL: true,           // dùng WSS
  userName: MQTT_USER,
  password: MQTT_PASS,
  keepAliveInterval: 60,
  cleanSession: true,
  onSuccess: onConnect,
  onFailure: function (res) {
    statusDiv.textContent = "Kết nối thất bại: " + (res?.errorMessage || "");
    statusDiv.className = "status-disconnected";
  }
};

client.connect(connectOptions);

//Handle MQTT
function onConnect() {
  statusDiv.textContent = "Đã kết nối";
  statusDiv.className = "status-connected";
  client.subscribe(MQTT_LOG_TOPIC);
  logList.innerHTML = "";
}

function onConnectionLost(res) {
  if (res.errorCode !== 0) {
    statusDiv.textContent = "Mất kết nối. Đang thử lại...";
    statusDiv.className = "status-disconnected";
    setTimeout(() => client.connect(connectOptions), 5000);
  }
}

function onMessageArrived(message) {
  if (message.destinationName !== MQTT_LOG_TOPIC) return;

  try {
    const data = JSON.parse(message.payloadString);
    const user = data.user || "Unknown";
    const action = data.action || "Unknown";
    const ts = new Date().toLocaleString("vi-VN");

    const li = document.createElement("li");
    li.textContent = `[${ts}] ${user} - ${action}`;
    logList.prepend(li);

    if (logList.children.length > 50) {
      logList.removeChild(logList.lastChild);
    }
  } catch (e) {
    console.error("Parse JSON lỗi:", e, "payload:", message.payloadString);
  }
}

//Them User
function addUser() {
  const pass = document.getElementById("add-pass").value.trim();
  const name = document.getElementById("add-name").value.trim();

  // validate tối thiểu, không popup
  if (!pass || !name || pass.length !== 6 || !/^\d+$/.test(pass)) {
    statusDiv.textContent = "Dữ liệu không hợp lệ (pass 6 chữ số, có tên).";
    statusDiv.className = "status-disconnected";
    return;
  }
  if (!client.isConnected()) {
    statusDiv.textContent = "Chưa kết nối MQTT.";
    statusDiv.className = "status-disconnected";
    return;
  }

  const msg = new Paho.MQTT.Message(JSON.stringify({
    command: "add_user",
    pass: pass,
    name: name
  }));
  msg.destinationName = MQTT_CMD_TOPIC;
  msg.qos = 1;
  msg.retained = false;

  try {
    client.send(msg);
    statusDiv.textContent = "Đã gửi lệnh add_user.";
    statusDiv.className = "status-connected";
    document.getElementById("add-pass").value = "";
    document.getElementById("add-name").value = "";
  } catch (err) {
    statusDiv.textContent = "Gửi lệnh thất bại.";
    statusDiv.className = "status-disconnected";
    console.error(err);
  }
}

//Xoa User
function deleteUser() {
  const pass = document.getElementById("del-pass").value.trim();

  if (!pass || pass.length !== 6 || !/^\d+$/.test(pass)) {
    statusDiv.textContent = "Dữ liệu không hợp lệ (pass 6 chữ số).";
    statusDiv.className = "status-disconnected";
    return;
  }
  if (!client.isConnected()) {
    statusDiv.textContent = "Chưa kết nối MQTT.";
    statusDiv.className = "status-disconnected";
    return;
  }

  const msg = new Paho.MQTT.Message(JSON.stringify({
    command: "del_user",
    pass: pass
  }));
  msg.destinationName = MQTT_CMD_TOPIC;
  msg.qos = 1;
  msg.retained = false;

  try {
    client.send(msg);
    statusDiv.textContent = "Đã gửi lệnh del_user.";
    statusDiv.className = "status-connected";
    document.getElementById("del-pass").value = "";
  } catch (err) {
    statusDiv.textContent = "Gửi lệnh thất bại.";
    statusDiv.className = "status-disconnected";
    console.error(err);
  }
}

//xu ly su kien
btnAdd.addEventListener("click", addUser);
btnDel.addEventListener("click", deleteUser);

document.getElementById("add-name").addEventListener("keypress", function (e) {
  if (e.key === "Enter") addUser();
});
document.getElementById("del-pass").addEventListener("keypress", function (e) {
  if (e.key === "Enter") deleteUser();
});
