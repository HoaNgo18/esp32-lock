
// Backend: Node.js + Express + MongoDB + MQTT
const express = require('express');
const mongoose = require('mongoose');
const mqtt = require('mqtt');
const cors = require('cors');
const bodyParser = require('body-parser');
const os = require('os'); // [FIX] Missing import


// 1. CẤU HÌNH
const MQTT_SERVER = 'mqtt://192.168.137.1'; // IP máy tính của bạn
const MQTT_PORT = 1883; // Port TCP thường (Backend dùng TCP, không phải WebSocket)
const MONGO_URI = 'mongodb://localhost:27017/smartlock'; // Đảm bảo MongoDB đang chạy
const PORT = 3001;
const path = require('path');
const dgram = require('dgram'); // [NEW] UDP Library

// [NEW] UDP Broadcast Config
const UDP_PORT = 12345;
const udpServer = dgram.createSocket('udp4');


// Topic (Wildcard + để nhận từ tất cả các khóa)
const TOPIC_LOG = 'lock/+/log';  // lock/{lock_id}/log
const TOPIC_CMD = 'lock/+/cmd';  // lock/{lock_id}/cmd

// 2. KẾT NỐI MONGODB
mongoose.connect(MONGO_URI)
    .then(() => console.log('MongoDB Connected'))
    .catch(err => console.error('MongoDB Error:', err));

// --- Schemas ---
const LogSchema = new mongoose.Schema({
    user: String,
    action: String,
    lock_id: String,
    date: String,  // YYYY-MM-DD format for easy filtering
    timestamp: { type: Date, default: Date.now }
});
// Index for faster date queries
LogSchema.index({ lock_id: 1, date: 1 });
const Log = mongoose.model('Log', LogSchema);

const UserSchema = new mongoose.Schema({
    username: String,
    pass: String,
    lock_id: String,  // Lock ID mà user thuộc về
    type: { type: String, default: 'user' }, // 'user' hoặc 'otp'
    createdAt: { type: Date, default: Date.now }
});
// Unique constraint: pass + lock_id (cùng pass có thể tồn tại ở các lock khác nhau)
UserSchema.index({ pass: 1, lock_id: 1 }, { unique: true });
const User = mongoose.model('User', UserSchema);

// 3. KẾT NỐI MQTT (Để "nghe trộm" dữ liệu lưu vào DB)
const mqttClient = mqtt.connect(MQTT_SERVER, { port: MQTT_PORT });

mqttClient.on('connect', () => {
    console.log('Backend Connected to MQTT Broker');
    mqttClient.subscribe([TOPIC_LOG, TOPIC_CMD], (err) => {
        if (!err) console.log(`Subscribed to ${TOPIC_LOG} & ${TOPIC_CMD}`);
    });
});

mqttClient.on('message', async (topic, message) => {
    try {
        const payload = JSON.parse(message.toString());

        // Parse lock_id từ topic: lock/{lock_id}/log hoặc lock/{lock_id}/cmd
        const topicParts = topic.split('/');
        const lockIdFromTopic = topicParts[1];
        const topicType = topicParts[2]; // 'log' hoặc 'cmd'

        // A. Xử lý lưu Log hoạt động
        if (topicType === 'log') {
            const now = new Date();
            const dateStr = now.toISOString().split('T')[0]; // YYYY-MM-DD
            const newLog = new Log({
                user: payload.user,
                action: payload.action,
                lock_id: payload.lock_id || lockIdFromTopic,
                date: dateStr
            });
            await newLog.save();
            console.log(`[DB-LOG][Lock ${lockIdFromTopic}] Saved: ${payload.user} - ${payload.action}`);
        }
        // Kiểm tra nếu là sự kiện xóa OTP từ thiết bị
        if (payload.action && payload.action.startsWith('otp_deleted:')) {
            const usedOtp = payload.action.split(':')[1];
            await User.findOneAndDelete({ pass: usedOtp, lock_id: lockIdFromTopic });
            console.log(`[DB-OTP][Lock ${lockIdFromTopic}] Auto-deleted used OTP: ${usedOtp}`);
        }

        // B. Xử lý đồng bộ User Database khi có lệnh Add/Delete
        if (topicType === 'cmd') {
            const cmd = payload.command;

            if (cmd === 'add_user') {
                await User.findOneAndUpdate(
                    { pass: payload.pass, lock_id: lockIdFromTopic },
                    { username: payload.username, type: 'user', lock_id: lockIdFromTopic },
                    { upsert: true, new: true }
                );
                console.log(`[DB-USER][Lock ${lockIdFromTopic}] Added/Updated: ${payload.username}`);
            }
            else if (cmd === 'del_user') {
                await User.findOneAndDelete({ pass: payload.pass, lock_id: lockIdFromTopic });
                console.log(`[DB-USER][Lock ${lockIdFromTopic}] Deleted pass: ${payload.pass}`);
            }
            else if (cmd === 'add_otp') {
                await User.create({
                    username: 'GUEST (OTP)',
                    pass: payload.pass,
                    lock_id: lockIdFromTopic,
                    type: 'otp'
                });
                console.log(`[DB-OTP][Lock ${lockIdFromTopic}] Created OTP`);
            }
        }

    } catch (e) {
        console.error('MQTT Message Error:', e);
    }
});

// 4. REST API (Cho Frontend gọi)
const app = express();
app.use(cors()); // Cho phép Frontend gọi API
app.use(bodyParser.json());

// Cho phép thư mục frontend được truy cập công khai
app.use(express.static(path.join(__dirname, '../frontend')));

// Khi truy cập trang chủ, gửi file index.html về cho điện thoại
app.get('/', (req, res) => {
    res.sendFile(path.join(__dirname, '../frontend/index.html'));
});

// API: Lấy danh sách Logs (Mới nhất trước)
app.get('/api/logs', async (req, res) => {
    try {
        const logs = await Log.find().sort({ timestamp: -1 }).limit(50);
        res.json(logs);
    } catch (e) {
        res.status(500).json({ error: e.message });
    }
});

// API: Lấy danh sách Logs theo Lock ID
app.get('/api/logs/:lockId', async (req, res) => {
    try {
        const logs = await Log.find({ lock_id: req.params.lockId })
            .sort({ timestamp: -1 }).limit(50);
        res.json(logs);
    } catch (e) {
        res.status(500).json({ error: e.message });
    }
});

// API: Lấy danh sách Logs theo Lock ID và Date (YYYY-MM-DD)
app.get('/api/logs/:lockId/:date', async (req, res) => {
    try {
        // Tạo khoảng thời gian từ 00:00:00 đến 23:59:59 của ngày được chọn
        const startOfDay = new Date(req.params.date + 'T00:00:00.000Z');
        const endOfDay = new Date(req.params.date + 'T23:59:59.999Z');

        const logs = await Log.find({
            lock_id: req.params.lockId,
            timestamp: { $gte: startOfDay, $lte: endOfDay }
        }).sort({ timestamp: -1 });
        res.json(logs);
    } catch (e) {
        res.status(500).json({ error: e.message });
    }
});

// API: Lấy danh sách Users
app.get('/api/users', async (req, res) => {
    try {
        const users = await User.find().sort({ createdAt: -1 });
        res.json(users);
    } catch (e) {
        res.status(500).json({ error: e.message });
    }
});

// API: Lấy danh sách Users theo Lock ID
app.get('/api/users/:lockId', async (req, res) => {
    try {
        const users = await User.find({ lock_id: req.params.lockId })
            .sort({ createdAt: -1 });
        res.json(users);
    } catch (e) {
        res.status(500).json({ error: e.message });
    }
});

app.listen(PORT, () => {
    console.log(`API Server running at http://localhost:${PORT}`);

    // [NEW] Start UDP Broadcast
    startUdpBroadcast();
});

// [NEW] UDP Broadcasting Function
function startUdpBroadcast() {
    udpServer.bind(() => {
        udpServer.setBroadcast(true);
        console.log(`UDP Broadcast active on port ${UDP_PORT}`);

        setInterval(() => {
            const message = Buffer.from(`ESP32_LOCK_BROKER_HERE|${MQTT_PORT}`);

            // 1. Broadcast Global
            udpServer.send(message, 0, message.length, UDP_PORT, '255.255.255.255');

            // 2. Broadcast on all interfaces (Fix for Windows Hotspot)
            const interfaces = os.networkInterfaces();

            for (const name of Object.keys(interfaces)) {
                for (const net of interfaces[name]) {
                    // Skip internal and non-IPv4
                    if (net.family === 'IPv4' && !net.internal) {
                        // Calculate broadcast address (naive approach for /24 subnets)
                        // Most Windows Hotspots are 192.168.137.x/24 -> 192.168.137.255
                        const parts = net.address.split('.');
                        parts[3] = '255';
                        const broadcastAddr = parts.join('.');

                        udpServer.send(message, 0, message.length, UDP_PORT, broadcastAddr, (err) => {
                            if (err && err.code !== 'ENETUNREACH') console.error('UDP Broadcast Error:', err);
                        });
                    }
                }
            }
        }, 3000);
    });
}
