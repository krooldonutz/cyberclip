const express = require('express');
const http = require('http');
const socketIo = require('socket.io');
const SerialPort = require('serialport').SerialPort;
const { ReadlineParser } = require('@serialport/parser-readline');
const sharp = require('sharp');
const gif = require('gif-frames');
const path = require('path');
const fs = require('fs');

const app = express();
const server = http.createServer(app);
const io = socketIo(server, {
  cors: { origin: "*" }
});

app.use(express.static('public'));
app.use(express.json({ limit: '50mb' }));

let port = null;
let parser = null;

// Store current display config
let displayConfig = {
  width: 32,
  height: 8,
  brightness: 255
};

// ============ SERIAL PORT MANAGEMENT ============
async function listSerialPorts() {
  try {
    const ports = await SerialPort.list();
    return ports;
  } catch (err) {
    console.error('Error listing ports:', err);
    return [];
  }
}

async function connectToESP(portPath, baudRate = 115200) {
  return new Promise((resolve, reject) => {
    port = new SerialPort({
      path: portPath,
      baudRate: baudRate,
      autoOpen: false
    });

    port.open((err) => {
      if (err) {
        reject(err);
        return;
      }

      parser = port.pipe(new ReadlineParser({ delimiter: '\n' }));
      
      parser.on('data', (data) => {
        console.log('ESP Response:', data.toString().trim());
        io.emit('esp_response', { data: data.toString().trim() });
      });

      port.on('error', (err) => {
        console.error('Port error:', err);
        io.emit('connection_error', { error: err.message });
      });

      resolve();
    });
  });
}

function disconnectFromESP() {
  if (port && port.isOpen) {
    port.close();
  }
}

// ============ IMAGE PROCESSING ============
async function processImageToMatrix(imagePath, width = 32, height = 8, brightness = 255) {
  try {
    // Convert image to specified dimensions and grayscale
    const processedImage = await sharp(imagePath)
      .resize(width, height, {
        fit: 'cover',
        position: 'center'
      })
      .grayscale()
      .raw()
      .toBuffer();

    // Convert to matrix format (brightness thresholding)
    const matrix = [];
    for (let i = 0; i < processedImage.length; i++) {
      const pixelBrightness = processedImage[i];
      matrix.push(pixelBrightness > 127 ? 255 : 0);
    }

    // Create protocol packet
    const packet = createMatrixPacket(matrix, width, height);
    return packet;
  } catch (err) {
    throw new Error(`Image processing failed: ${err.message}`);
  }
}

async function processGifToFrames(gifPath, width = 32, height = 8) {
  try {
    const frames = await gif({ url: gifPath, frames: 'all', cumulative: false });
    
    const matrixFrames = [];
    for (const frame of frames) {
      const canvas = frame.getImage();
      const image = await sharp(canvas.toBuffer('image/png'))
        .resize(width, height, {
          fit: 'cover',
          position: 'center'
        })
        .grayscale()
        .raw()
        .toBuffer();

      const matrix = [];
      for (let i = 0; i < image.length; i++) {
        matrix.push(image[i] > 127 ? 255 : 0);
      }
      matrixFrames.push(matrix);
    }

    return matrixFrames;
  } catch (err) {
    throw new Error(`GIF processing failed: ${err.message}`);
  }
}

// ============ PROTOCOL PACKETS ============
function createMatrixPacket(matrix, width, height) {
  // Protocol: [START:0xFF][CMD:0x01][WIDTH][HEIGHT][DATA...]
  const packet = Buffer.alloc(4 + matrix.length);
  
  packet[0] = 0xFF;        // Start byte
  packet[1] = 0x01;        // Command: Display matrix
  packet[2] = width;
  packet[3] = height;
  
  for (let i = 0; i < matrix.length; i++) {
    packet[4 + i] = matrix[i];
  }
  
  return packet;
}

function createAnimationPacket(frameCount, delayMs) {
  // Protocol: [START:0xFF][CMD:0x02][FRAMES][DELAY_H][DELAY_L]
  const packet = Buffer.alloc(5);
  packet[0] = 0xFF;
  packet[1] = 0x02;
  packet[2] = frameCount;
  packet[3] = (delayMs >> 8) & 0xFF;
  packet[4] = delayMs & 0xFF;
  return packet;
}

// ============ API ENDPOINTS ============
app.get('/api/ports', async (req, res) => {
  const ports = await listSerialPorts();
  res.json(ports);
});

app.post('/api/connect', async (req, res) => {
  try {
    const { portPath, baudRate } = req.body;
    await connectToESP(portPath, baudRate);
    res.json({ success: true, message: 'Connected to ESP' });
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

app.post('/api/disconnect', (req, res) => {
  disconnectFromESP();
  res.json({ success: true, message: 'Disconnected from ESP' });
});

app.post('/api/display-image', async (req, res) => {
  try {
    const { imagePath } = req.body;
    
    if (!fs.existsSync(imagePath)) {
      return res.status(400).json({ error: 'File not found' });
    }

    const packet = await processImageToMatrix(imagePath, displayConfig.width, displayConfig.height);
    
    if (port && port.isOpen) {
      port.write(packet, (err) => {
        if (err) return res.status(500).json({ error: err.message });
        res.json({ success: true, message: 'Image sent to ESP' });
      });
    } else {
      res.status(500).json({ error: 'ESP not connected' });
    }
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

app.post('/api/display-gif', async (req, res) => {
  try {
    const { gifPath, delayMs = 100 } = req.body;
    
    if (!fs.existsSync(gifPath)) {
      return res.status(400).json({ error: 'File not found' });
    }

    const frames = await processGifToFrames(gifPath, displayConfig.width, displayConfig.height);
    
    if (!port || !port.isOpen) {
      return res.status(500).json({ error: 'ESP not connected' });
    }

    // Send animation start command
    const animPacket = createAnimationPacket(frames.length, delayMs);
    port.write(animPacket);

    // Send each frame with delay
    let frameIndex = 0;
    const sendNextFrame = () => {
      if (frameIndex < frames.length) {
        const packet = createMatrixPacket(frames[frameIndex], displayConfig.width, displayConfig.height);
        port.write(packet, (err) => {
          if (err) console.error('Frame send error:', err);
          frameIndex++;
          setTimeout(sendNextFrame, delayMs);
        });
      }
    };

    sendNextFrame();
    res.json({ success: true, frameCount: frames.length });
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

app.post('/api/config', (req, res) => {
  displayConfig = { ...displayConfig, ...req.body };
  res.json({ success: true, config: displayConfig });
});

// ============ WEBSOCKET ============
io.on('connection', (socket) => {
  console.log('Client connected');

  socket.on('disconnect', () => {
    console.log('Client disconnected');
  });
});

// ============ START SERVER ============
const PORT = process.env.PORT || 3000;
server.listen(PORT, () => {
  console.log(`Server running on http://localhost:${PORT}`);
});

process.on('SIGINT', () => {
  disconnectFromESP();
  process.exit();
});
