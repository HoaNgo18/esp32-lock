
// Backend: Node.js + Express + MongoDB + MQTT
const express = require('express');
const mongoose = require('mongoose');
const mqtt = require('mqtt');
const cors = require('cors');
const bodyParser = require('body-parser');

// 1. CẤU HÌNH
const MQTT_SERVER = 'mqtt://192.168.1.7'; // IP máy tính của bạn
const MQTT_PORT = 1883; // Port TCP thường (Backend dùng TCP, không phải WebSocket)
const MONGO_URI = 'mongodb://localhost:27017/smartlock'; // Đảm bảo MongoDB đang chạy
const PORT = 3001;

// Topic
const TOPIC_LOG = 'lock/log';
const TOPIC_CMD = 'lock/cmd';

// 2. KẾT NỐI MONGODB
mongoose.connect(MONGO_URI)
    .then(() => console.log('MongoDB Connected'))
    .catch(err => console.error('MongoDB Error:', err));

// --- Schemas ---
const LogSchema = new mongoose.Schema({
    user: String,
    action: String,
    lock_id: String,
    timestamp: { type: Date, default: Date.now }
});
const Log = mongoose.model('Log', LogSchema);

const UserSchema = new mongoose.Schema({
    username: String,
    pass: { type: String, unique: true }, // Pass là định danh duy nhất trong hệ thống này
    type: { type: String, default: 'user' }, // 'user' hoặc 'otp'
    createdAt: { type: Date, default: Date.now }
});
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

        // A. Xử lý lưu Log hoạt động
        if (topic === TOPIC_LOG) {
            const newLog = new Log({
                user: payload.user,
                action: payload.action,
                lock_id: payload.lock_id || "2"
            });
            await newLog.save();
            console.log(`[DB-LOG] Saved: ${payload.user} - ${payload.action}`);
        }
        // Kiểm tra nếu là sự kiện xóa OTP từ thiết bị
        if (payload.action && payload.action.startsWith('otp_deleted:')) {
            const usedOtp = payload.action.split(':')[1]; // Lấy mã phía sau dấu hai chấm
            await User.findOneAndDelete({ pass: usedOtp });
            console.log(`[DB-OTP] Auto-deleted used OTP: ${usedOtp}`);
        }

        // B. Xử lý đồng bộ User Database khi có lệnh Add/Delete
        // Lưu ý: Ta bắt lệnh CMD để lấy password (vì Log không chứa pass)
        if (topic === TOPIC_CMD) {
            const cmd = payload.command;

            if (cmd === 'add_user') {
                // Upsert: Nếu chưa có thì thêm, có rồi thì update tên
                await User.findOneAndUpdate(
                    { pass: payload.pass },
                    { username: payload.username, type: 'user' },
                    { upsert: true, new: true }
                );
                console.log(`[DB-USER] Added/Updated: ${payload.username}`);
            }
            else if (cmd === 'del_user') {
                await User.findOneAndDelete({ pass: payload.pass });
                console.log(`[DB-USER] Deleted pass: ${payload.pass}`);
            }
            else if (cmd === 'add_otp') {
                // OTP thường dùng 1 lần, nhưng vẫn lưu để biết
                await User.create({
                    username: 'GUEST (OTP)',
                    pass: payload.pass,
                    type: 'otp'
                });
                console.log(`[DB-OTP] Created OTP`);
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

// API: Lấy danh sách Logs (Mới nhất trước)
app.get('/api/logs', async (req, res) => {
    try {
        const logs = await Log.find().sort({ timestamp: -1 }).limit(50);
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

app.listen(PORT, () => {
    console.log(`API Server running at http://localhost:${PORT}`);
});