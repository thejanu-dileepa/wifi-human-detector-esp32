const express = require('express');
const http = require('http');
const { Server } = require('socket.io');
const { SerialPort } = require('serialport');
const { ReadlineParser } = require('@serialport/parser-readline');

const app = express();
const server = http.createServer(app);

// Setup Socket.IO to allow connections from the React frontend
const io = new Server(server, {
    cors: {
        origin: "*", 
        methods: ["GET", "POST"]
    }
});

// Serial Port Configuration
const PORT_NAME = 'COM3'; // Make sure this matches your ESP32 Port
const BAUD_RATE = 115200;

// Initialize the Serial Port
const port = new SerialPort({ path: PORT_NAME, baudRate: BAUD_RATE });
const parser = port.pipe(new ReadlineParser({ delimiter: '\r\n' }));

port.on('open', () => {
    console.log(`Serial Port ${PORT_NAME} opened successfully.`);
});

// When data is received from the ESP32
parser.on('data', (data) => {
    // Check if the incoming data contains commas (our 64 sub-carrier values)
    if (data.includes(',')) {
        // Convert the string "12.5,4.3,..." into an array of numbers
        const csiArray = data.split(',').map(Number);
        
        // Broadcast the array to the web frontend in real-time
        io.emit('csi_data', csiArray);
    } else {
        // Print other text messages (like "Connecting to Router...") to the terminal
        console.log("ESP32 Msg:", data);
    }
});

// Error handling for the Serial Port
port.on('error', (err) => {
    console.error('Serial Port Error: ', err.message);
});

// Start the Node.js server on port 3001
server.listen(3001, () => {
    console.log('Socket.IO Data Bridge running on http://localhost:3001');
});