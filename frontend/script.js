// CONFIGURATION
// ============================================
// [FIX] Tự động lấy IP từ thanh địa chỉ (để chạy được trên cả điện thoại)
const SERVER_IP = window.location.hostname || "localhost";

const MQTT_SERVER = SERVER_IP;
const API_URL = `http://${SERVER_IP}:3001/api`; // Địa chỉ Backend Node.js


// Lưu ý: Web dùng WebSockets 
const MQTT_PORT = 9001;
const MQTT_PATH = "/mqtt";
const MQTT_USER = ""; // Để trống (vì allow_anonymous true)
const MQTT_PASS = ""; // Để trống

const MQTT_CMD_TOPIC = "lock/cmd";
const MQTT_LOG_TOPIC = "lock/log";

const WEB_ADMIN_PASS = "admin123";
const MQTT_CLIENT_ID = "web_admin_" + Math.floor(Math.random() * 100000);

// ============================================
// DOM ELEMENTS
// ============================================
const loginSection = document.getElementById("login-section");
const dashboardSection = document.getElementById("dashboard-section");
const statusDiv = document.getElementById("status");
const logList = document.getElementById("log-list");
const userTableBody = document.getElementById("user-table-body");

// Modals
const modalOverlay = document.getElementById("modal-overlay");
const modalRemote = document.getElementById("modal-remote"); // NEW
const modalAdd = document.getElementById("modal-add");
const modalDel = document.getElementById("modal-del");
const modalOtp = document.getElementById("modal-otp");

// Inputs
const inpAddUser = document.getElementById("inp-add-username");
const inpAddPass = document.getElementById("inp-add-pass");
const inpDelPass = document.getElementById("inp-del-pass");
const otpValueDisplay = document.getElementById("otp-value");

// ============================================
// STATE MANAGEMENT
// ============================================
let client = null;
let reconnectAttempts = 0;
const maxReconnectAttempts = 10;
let isIntentionalDisconnect = false;
let currentOtp = "";
let currentLockId = localStorage.getItem("currentLockId") || "1";  // Lock đang được chọn
let currentDate = new Date().toISOString().split('T')[0];  // Ngày hiện tại (YYYY-MM-DD)

// Dynamic MQTT Topics based on selected lock
const getMQTTCmdTopic = () => `lock/${currentLockId}/cmd`;
const getMQTTLogTopic = () => `lock/${currentLockId}/log`;

// ============================================
// LOGIN LOGIC
// ============================================
function init() {
  const isLoggedIn = localStorage.getItem("isLoggedIn");
  if (isLoggedIn === "true") {
    showDashboard();
  } else {
    showLogin();
  }
}

function showLogin() {
  loginSection.classList.remove("hidden");
  dashboardSection.classList.add("hidden");
  isIntentionalDisconnect = true;
  if (client && client.isConnected()) {
    client.disconnect();
  }
  client = null;
}

function showDashboard() {
  loginSection.classList.add("hidden");
  dashboardSection.classList.remove("hidden");
  isIntentionalDisconnect = false;
  reconnectAttempts = 0;

  // KẾT NỐI MQTT (Để nhận sự kiện real-time)
  connectMQTT();

  // GỌI API (Để lấy dữ liệu lịch sử từ MongoDB)
  loadDataFromBackend();
}

document.getElementById("btn-login").addEventListener("click", () => {
  const inputPass = document.getElementById("login-pass").value;
  const errorMsg = document.getElementById("login-error");

  if (inputPass === WEB_ADMIN_PASS) {
    localStorage.setItem("isLoggedIn", "true");
    errorMsg.textContent = "";
    showDashboard();
  } else {
    errorMsg.textContent = "❌ Wrong password! Please try again.";
  }
});

document.getElementById("login-pass").addEventListener("keypress", (e) => {
  if (e.key === "Enter") document.getElementById("btn-login").click();
});

document.getElementById("btn-logout").addEventListener("click", () => {
  localStorage.removeItem("isLoggedIn");
  showLogin();
});

// ============================================
// MQTT LOGIC
// ============================================
function connectMQTT() {
  if (client && client.isConnected()) {
    console.log("Already connected to MQTT");
    return;
  }

  statusDiv.textContent = "Connecting...";
  statusDiv.className = "status-badge connecting";

  client = new Paho.MQTT.Client(MQTT_SERVER, Number(MQTT_PORT), MQTT_PATH, MQTT_CLIENT_ID);
  client.onConnectionLost = onConnectionLost;
  client.onMessageArrived = onMessageArrived;

  const connectOptions = {
    timeout: 30,
    useSSL: false,
    userName: MQTT_USER,
    password: MQTT_PASS,
    keepAliveInterval: 60,
    cleanSession: true,
    onSuccess: onConnect,
    onFailure: function (res) {
      console.error("MQTT Connection failed:", res);
      statusDiv.textContent = "Connection Failed";
      statusDiv.className = "status-badge disconnected";

      if (!isIntentionalDisconnect && reconnectAttempts < maxReconnectAttempts) {
        reconnectAttempts++;
        setTimeout(() => {
          if (!isIntentionalDisconnect) connectMQTT();
        }, 3000 * reconnectAttempts);
      }
    }
  };

  try {
    client.connect(connectOptions);
  } catch (e) {
    console.error("Error connecting:", e);
    statusDiv.textContent = "Connection Error";
    statusDiv.className = "status-badge disconnected";
  }
}

function onConnect() {
  console.log("MQTT Connected successfully");
  reconnectAttempts = 0;
  statusDiv.textContent = "Connected";
  statusDiv.className = "status-badge connected";

  try {
    client.subscribe(getMQTTLogTopic(), { qos: 1 });
    addLog("System", `Connected to Lock #${currentLockId}`, "success");
  } catch (e) {
    console.error("Subscribe error:", e);
  }
}

function onConnectionLost(res) {
  console.log("Connection lost:", res);
  if (!isIntentionalDisconnect) {
    statusDiv.textContent = "Reconnecting...";
    statusDiv.className = "status-badge disconnected";
    setTimeout(() => connectMQTT(), 5000);
  }
}

function onMessageArrived(message) {
  // 1. Chỉ xử lý tin nhắn thuộc topic Log của lock hiện tại
  if (message.destinationName !== getMQTTLogTopic()) return;

  try {
    const data = JSON.parse(message.payloadString);
    const user = data.user || "Unknown";
    const action = data.action || "unknown_action";

    // LOGIC ĐỒNG BỘ DỮ LIỆU (DATA SYNC)

    // Xác định các hành động làm thay đổi dữ liệu trong Database
    // (Bao gồm: Thêm mới, Xóa User, hoặc OTP tự hủy sau khi dùng)
    const isDataChanged =
      action.includes("User added") ||
      action.includes("User deleted") ||
      action.includes("otp_deleted") ||
      action === "created_otp";

    // Nếu phát hiện dữ liệu thay đổi -> Gọi API load lại toàn bộ bảng User
    if (isDataChanged) {
      console.log(`[Sync] Data changed via MQTT (${action}) -> Reloading table...`);
      setTimeout(() => {
        loadDataFromBackend();
      }, 500);
    }

    // LOGIC HIỂN THỊ LOG (UI ACTIVITY)

    // Phân loại log để gán màu sắc hiển thị (CSS Class)
    let logType = "";
    if (action.includes("unlocked") || action.includes("otp_used") || action.includes("added")) {
      logType = "success";
    } else if (action.includes("failed") || action.includes("lockout")) {
      logType = "error";
    } else if (action.includes("locked") || action.includes("deleted")) {
      logType = "warning";
    }

    // Luôn hiển thị dòng log sự kiện ra màn hình để Admin nắm bắt ngay
    addLog(`${user}`, action, logType);

  } catch (e) {
    console.error("MQTT Message Error:", e);
  }
}

function sendCommand(cmd, payloadObj = {}) {
  if (!client || !client.isConnected()) {
    alert("MQTT not connected! Please wait...");
    return false;
  }

  try {
    const payload = JSON.stringify({ command: cmd, ...payloadObj });
    const msg = new Paho.MQTT.Message(payload);
    msg.destinationName = getMQTTCmdTopic();
    msg.qos = 1;
    msg.retained = false;
    client.send(msg);
    return true;
  } catch (e) {
    alert("Failed to send command: " + e.message);
    return false;
  }
}

// ============================================
// UI HELPER FUNCTIONS
// ============================================
function addLog(user, action, type = "") {
  const timeStamp = new Date().toLocaleTimeString("vi-VN");
  const li = document.createElement("li");
  li.className = type;
  li.innerHTML = `<strong>[${timeStamp}]</strong> ${user}: ${action}`;

  if (logList.children[0]?.textContent.includes("Waiting for events")) {
    logList.innerHTML = "";
  }
  logList.prepend(li);
}

function addUserToTable(username, pass, type) {
  if (userTableBody.children[0]?.textContent.includes("Loading")) {
    userTableBody.innerHTML = "";
  }
  const existingRows = userTableBody.querySelectorAll("tr");
  for (let row of existingRows) {
    if (row.cells[0].textContent === username) return;
  }

  const tr = document.createElement("tr");
  tr.innerHTML = `
    <td>${username}</td>
    <td>${pass}</td>
    <td style="text-align: right;">
        <span class="tag ${type === 'otp' ? 'otp' : 'active'}" 
              style="padding: 2px 8px; border-radius: 4px; background: ${type === 'otp' ? '#fbbf24' : '#34d399'}; color: #000; font-weight: bold; font-size: 0.7rem;">
            ${type === 'otp' ? 'OTP' : 'User'}
        </span>
    </td>`;
  userTableBody.appendChild(tr);
}

function removeUserFromTable(username) {
  const rows = userTableBody.querySelectorAll("tr");
  rows.forEach(row => {
    if (row.cells[0]?.textContent === username) row.remove();
  });
}

function filterUserTable() {
  const filter = document.getElementById("search-user").value.toUpperCase();
  const rows = userTableBody.getElementsByTagName("tr");
  for (let i = 0; i < rows.length; i++) {
    const td = rows[i].getElementsByTagName("td")[0];
    if (td) {
      const txtValue = td.textContent || td.innerText;
      if (txtValue.toUpperCase().indexOf(filter) > -1) {
        rows[i].style.display = "";
      } else {
        rows[i].style.display = "none";
      }
    }
  }
}

async function loadDataFromBackend() {
  try {
    // 1. Tải Logs theo lock_id và date
    const logRes = await fetch(`${API_URL}/logs/${currentLockId}/${currentDate}`);
    const logs = await logRes.json();

    // Xóa thông báo "Waiting..." cũ
    const logList = document.getElementById("log-list");
    logList.innerHTML = "";

    if (logs.length === 0) {
      logList.innerHTML = '<li class="text-center text-muted">No logs for this date</li>';
    }

    logs.forEach(log => {
      // Xác định loại log để tô màu
      let logType = "";
      const action = log.action;
      if (action.includes("unlocked") || action.includes("otp_used") || action.includes("added")) logType = "success";
      else if (action.includes("failed") || action.includes("lockout")) logType = "error";
      else if (action.includes("locked") || action.includes("deleted")) logType = "warning";

      // Format thời gian từ ISO string sang giờ Việt Nam
      const timeStamp = new Date(log.timestamp).toLocaleTimeString("vi-VN");

      const li = document.createElement("li");
      li.className = logType;
      li.innerHTML = `<strong>[${timeStamp}]</strong> ${log.user}: ${log.action}`;
      logList.appendChild(li); // Dùng append log cũ xuống dưới
    });

    // 2. Tải Users theo lock_id
    const userRes = await fetch(`${API_URL}/users/${currentLockId}`);
    const users = await userRes.json();

    const userTableBody = document.getElementById("user-table-body");
    userTableBody.innerHTML = ""; // Xóa "Loading..."

    users.forEach(u => {
      // Tận dụng hàm addUserToTable có sẵn nhưng sửa lại chút để không check trùng lặp nếu clear hết rồi
      const tr = document.createElement("tr");
      tr.innerHTML = `
                <td>${u.username}</td>
                <td>${u.pass}</td>
                <td style="text-align: right;">
                    <span class="tag ${u.type === 'otp' ? 'otp' : 'active'}" 
                          style="padding: 2px 8px; border-radius: 4px; background: ${u.type === 'otp' ? '#fbbf24' : '#34d399'}; color: #000; font-weight: bold; font-size: 0.7rem;">
                        ${u.type === 'otp' ? 'OTP' : 'User'}
                    </span>
                </td>`;
      userTableBody.appendChild(tr);
    });

  } catch (e) {
    console.error("Error loading backend data:", e);
    addLog("System", "Failed to load history from DB", "error");
  }
}

// ============================================
// MODAL & BUTTON HANDLERS
// ============================================

// 1. OPEN MODALS
document.getElementById("btn-show-remote").addEventListener("click", () => {
  // Open Remote Modal (Blue)
  modalOverlay.classList.remove("hidden");
  modalRemote.classList.remove("hidden");
});

document.getElementById("btn-show-add").addEventListener("click", () => {
  // Open Add User Modal (Green)
  inpAddUser.value = "";
  inpAddPass.value = "";
  modalOverlay.classList.remove("hidden");
  modalAdd.classList.remove("hidden");
});

document.getElementById("btn-show-del").addEventListener("click", () => {
  // Open Delete Modal (Red)
  inpDelPass.value = "";
  modalOverlay.classList.remove("hidden");
  modalDel.classList.remove("hidden");
});

document.getElementById("btn-show-otp").addEventListener("click", () => {
  // Open OTP Modal (Orange)
  currentOtp = Math.floor(100000 + Math.random() * 900000).toString();
  otpValueDisplay.textContent = currentOtp;
  modalOverlay.classList.remove("hidden");
  modalOtp.classList.remove("hidden");
});

// 2. CLOSE MODALS
function closeAllModals() {
  modalOverlay.classList.add("hidden");
  modalRemote.classList.add("hidden");
  modalAdd.classList.add("hidden");
  modalDel.classList.add("hidden");
  modalOtp.classList.add("hidden");
}

// 3. ACTION HANDLERS INSIDE MODALS

// --- REMOTE OPEN CONFIRM ---
document.getElementById("btn-confirm-remote").addEventListener("click", () => {
  if (sendCommand("remote_open", { user: "WebAdmin" })) {
    closeAllModals();
    addLog("Admin", `Remote open command sent`, "success");
  }
});

// --- ADD USER CONFIRM ---
document.getElementById("btn-confirm-add").addEventListener("click", () => {
  const username = inpAddUser.value.trim();
  const pass = inpAddPass.value.trim();

  if (!username || !pass) { alert("Please fill all fields"); return; }
  if (pass.length !== 6 || isNaN(pass)) { alert("Password must be exactly 6 digits"); return; }

  if (sendCommand("add_user", { pass: pass, username: username })) {
    closeAllModals();
    addLog("Admin", `Sent Add User command for ${username}`, "warning");
  }
});

// --- DELETE USER CONFIRM ---
document.getElementById("btn-confirm-del").addEventListener("click", () => {
  const pass = inpDelPass.value.trim();
  if (pass.length !== 6 || isNaN(pass)) { alert("Password must be 6 digits"); return; }

  if (sendCommand("del_user", { pass: pass })) {
    closeAllModals();
    addLog("Admin", `Sent Delete User command`, "warning");
  }
});

// --- SEND OTP CONFIRM ---
document.getElementById("btn-confirm-otp").addEventListener("click", () => {
  if (sendCommand("add_otp", { pass: currentOtp })) {
    closeAllModals();
    addLog("Admin", `Created OTP: ${currentOtp}`, "success");
  }
});

// ============================================
// LOCK SELECTOR HANDLER
// ============================================
function switchLock(lockId) {
  if (lockId === currentLockId) return;

  // Unsubscribe từ lock cũ
  if (client && client.isConnected()) {
    try {
      client.unsubscribe(getMQTTLogTopic());
    } catch (e) {
      console.error("Unsubscribe error:", e);
    }
  }

  // Cập nhật lock hiện tại
  currentLockId = lockId;
  localStorage.setItem("currentLockId", lockId);

  // Subscribe vào lock mới
  if (client && client.isConnected()) {
    try {
      client.subscribe(getMQTTLogTopic(), { qos: 1 });
      addLog("System", `Switched to Lock #${lockId}`, "success");
    } catch (e) {
      console.error("Subscribe error:", e);
    }
  }

  // Reload data cho lock mới
  loadDataFromBackend();
}

// Event listener cho lock selector
document.getElementById("lock-select").addEventListener("change", (e) => {
  switchLock(e.target.value);
});

// Event listener cho date picker
document.getElementById("log-date-picker").addEventListener("change", (e) => {
  currentDate = e.target.value;
  loadDataFromBackend();
});

// ============================================
// INITIALIZATION
// ============================================
function initSelectors() {
  // Set lock selector
  const lockSelect = document.getElementById("lock-select");
  if (lockSelect) {
    lockSelect.value = currentLockId;
  }

  // Set date picker to today
  const datePicker = document.getElementById("log-date-picker");
  if (datePicker) {
    datePicker.value = currentDate;
  }
}

// Gọi lại init để include selectors
const originalInit = init;
function init() {
  const isLoggedIn = localStorage.getItem("isLoggedIn");
  if (isLoggedIn === "true") {
    showDashboard();
    initSelectors();
  } else {
    showLogin();
  }
}

init();