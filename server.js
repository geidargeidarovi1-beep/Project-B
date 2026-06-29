const express = require('express');
const mqtt = require('mqtt');
const admin = require('firebase-admin');
const https = require('https');
const path = require('path');
const session = require('express-session'); 

const serviceAccount = require("./serviceAccountKey.json");
admin.initializeApp({
  credential: admin.credential.cert(serviceAccount),
  databaseURL: "https://fooddonate-a2e53-default-rtdb.firebaseio.com"
});

const db = admin.database();
const app = express();

app.use(express.json());
app.use(express.static(path.join(__dirname, 'public'), { charset: 'utf-8' }));
app.use(session({
    secret: 'admin-secret-key-1234',
    resave: false,
    saveUninitialized: true
}));

app.get('/', (req, res) => {
    res.sendFile(path.join(__dirname, 'public', 'index.html'));
});

app.get('/admin', (req, res) => {
    if (req.session.role === 'admin') {
        res.sendFile(path.join(__dirname, 'private', 'admin.html'));
    } else {
        res.redirect('/');
    }
});

const mqttOptions = {
    port: 8883,
    host: '83820ec0bcb54f2881a8900404db6e3c.s1.eu.hivemq.cloud',
    protocol: 'mqtts',
    username: 'esp32_user', 
    password: 'Admin1234!', 
    rejectUnauthorized: false 
};

const client = mqtt.connect(`mqtts://${mqttOptions.host}`, mqttOptions);

client.on('connect', () => {
    client.subscribe('rfid/request'); 
});

async function distributePoolMoney(totalAmount) {
    if (!totalAmount || totalAmount <= 0) return;
    
    const usersRef = db.ref('users');
    const snapshot = await usersRef.once('value');
    const users = snapshot.val();
    
    if (!users) return;

    const userIds = Object.keys(users);
    const regularUsers = userIds.filter(uid => users[uid].role !== "admin");
    const userCount = regularUsers.length;
    
    if (userCount === 0) return;

    const share = totalAmount / userCount;
    const updates = {};
    for (let uid of regularUsers) {
        const currentBalance = users[uid].balance || 0;
        updates[`${uid}/balance`] = currentBalance + share;
    }

    await usersRef.update(updates);
}

async function checkPinAndLimit(uid, pin) {
    const userRef = db.ref(`users/${uid}`);
    const snapshot = await userRef.once('value');
    const user = snapshot.val();
    const now = Date.now();
    const ONE_MINUTE_MS = 60 * 1000;

    if (!user || String(user.cardPin) !== String(pin)) {
        return { success: false, message: "ბარათი ან PIN არასწორია!", balance: 0 };
    }

    if (user.lastUsed) {
        const elapsedTime = now - new Date(user.lastUsed).getTime();
        if (elapsedTime < ONE_MINUTE_MS) {
            const remainingMs = ONE_MINUTE_MS - elapsedTime;
            const seconds = Math.ceil(remainingMs / 1000);
            return { success: false, message: `ლიმიტი! ${seconds}წმ შემდეგ.`, balance: user.balance };
        }
    }

    if (user.balance < 3) {
        return { success: false, message: "ბალანსი არ არის საკმარისი!", balance: user.balance };
    }

    return { success: true, balance: user.balance };
}

async function performDeduction(uid) {
    const userRef = db.ref(`users/${uid}`);
    const snapshot = await userRef.once('value');
    const user = snapshot.val();
    
    if(user && user.balance >= 3) {
        const newBalance = user.balance - 3;
        await userRef.update({ balance: newBalance, lastUsed: new Date().toISOString() });
    }
}

client.on('message', async (topic, message) => {
    if (topic === 'rfid/request') {
        try {
            const data = JSON.parse(message.toString());

            if (data.action === "auth") {
                const result = await checkPinAndLimit(data.uid, data.pin);
                client.publish('rfid/response', JSON.stringify(result));
            } 
            else if (data.action === "deduct") {
                await performDeduction(data.uid);
            }
            else if (data.action === "card_deposit" || data.action === "coin_deposit") {
                await distributePoolMoney(data.amount);
            }
        } catch (e) {
        }
    }
});

app.post('/api/login', async (req, res) => {
    const { username, webPassword } = req.body;
    try {
        const snapshot = await db.ref('users').orderByChild('username').equalTo(username).once('value');
        const users = snapshot.val();
        if (!users) return res.json({ success: false, message: "მომხმარებელი არ მოიძებნა!" }); 

        const uid = Object.keys(users)[0];
        const user = users[uid];
        if (user.webPassword !== webPassword) return res.json({ success: false, message: "პაროლი არასწორია!" }); 

        req.session.uid = uid;
        req.session.role = user.role;

        if (user.role === "admin") {
            return res.json({ success: true, role: "admin", redirect: "/admin" });
        }
        return res.json({ success: true, role: "user", uid: uid, balance: user.balance, lastUsed: user.lastUsed, cardPin: user.cardPin });
    } catch (err) {
        return res.json({ success: false, message: "სერვერის შეცდომა!" });
    }
});

app.post('/api/admin/add-user', async (req, res) => {
    const { uid, username, webPassword, cardPin, balance, role } = req.body;
    try {
        await db.ref(`users/${uid}`).set({
            username: username,
            webPassword: webPassword,
            cardPin: cardPin,
            balance: Number(balance),
            role: role || "user",
            lastUsed: null
        });
        res.json({ success: true });
    } catch (err) {
        res.json({ success: false });
    }
});

app.post('/api/change-pin', async (req, res) => {
    const { uid, newPin } = req.body;
    try {
        await db.ref(`users/${uid}`).update({ cardPin: newPin });
        res.json({ success: true, message: "PIN შეიცვალა!" });
    } catch (err) {
        res.json({ success: false, message: "შეცდომა!" });
    }
});

app.get('/api/admin/users', async (req, res) => {
    if (req.session.role !== 'admin') {
        return res.status(403).json({ error: "Unauthorized" });
    }
    const snapshot = await db.ref('users').once('value');
    res.json(snapshot.val() || {});
});

setInterval(() => {
    https.get("https://donatefofood.onrender.com/").on('error', (err) => {
    });
}, 10 * 60 * 1000); 

const PORT = process.env.PORT || 3000;
app.listen(PORT, () => {
});