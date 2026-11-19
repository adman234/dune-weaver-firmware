// XMODEM-CRC sender for FluidNC's `$Xmodem/Receive=<file>`.
//
// The receiver (FluidNC/src/xmodem.cpp) sends 'C' every 2 s, up to 16 times,
// then takes SOH (128-byte) or STX (1K) blocks with a CRC16-CCITT trailer,
// strips the 0x1A padding from the last block and ends on EOT -> ACK. We send
// 128-byte blocks: they are the most forgiving over a USB-UART bridge.
//
// `link` is anything with:
//   readByte(timeoutMs) -> Promise<number>  (-1 on timeout)
//   write(Uint8Array)   -> Promise<void>

const SOH = 0x01, EOT = 0x04, ACK = 0x06, CAN = 0x18, SUB = 0x1a;
const C = 0x43, LF = 0x0a;

export function crc16(bytes) {
  let crc = 0;
  for (const b of bytes) {
    crc ^= b << 8;
    for (let i = 0; i < 8; i++) {
      crc = crc & 0x8000 ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff;
    }
  }
  return crc;
}

export function makeBlocks(data) {
  const blocks = [];
  const count = Math.max(1, Math.ceil(data.length / 128));
  for (let i = 0; i < count; i++) {
    const n = (i + 1) & 0xff;
    const block = new Uint8Array(133);
    block[0] = SOH;
    block[1] = n;
    block[2] = 0xff - n;
    const body = block.subarray(3, 131);
    body.fill(SUB);
    body.set(data.subarray(i * 128, Math.min(data.length, (i + 1) * 128)));
    const crc = crc16(body);
    block[131] = crc >> 8;
    block[132] = crc & 0xff;
    blocks.push(block);
  }
  return blocks;
}

// Wait for the receiver's 'C'. It must start a line (or follow another 'C')
// so a 'C' inside a log line is not taken as the start signal.
async function waitForStart(link, timeoutMs) {
  const end = Date.now() + timeoutMs;
  let prev = LF;
  while (Date.now() < end) {
    const b = await link.readByte(Math.max(1, end - Date.now()));
    if (b < 0) break;
    if (b === C && (prev === LF || prev === C)) return;
    if (b === CAN) throw new Error("XMODEM: receiver cancelled");
    prev = b;
  }
  throw new Error("XMODEM: the board never asked for data");
}

async function drain(link, quietMs) {
  while ((await link.readByte(quietMs)) >= 0) { /* discard */ }
}

export async function sendFile(link, data, onProgress = () => {}) {
  if (!(data instanceof Uint8Array)) data = new TextEncoder().encode(String(data));
  await waitForStart(link, 35000);
  // Several 'C's may be queued behind the one we saw.
  await drain(link, 50);

  const blocks = makeBlocks(data);
  for (let i = 0; i < blocks.length; i++) {
    let sent = false;
    for (let attempt = 0; attempt < 10 && !sent; attempt++) {
      await link.write(blocks[i]);
      const r = await link.readByte(3000);
      if (r === ACK) sent = true;
      else if (r === CAN) throw new Error("XMODEM: receiver cancelled at block " + (i + 1));
      else if (r === C && i === 0) continue; // a late start request: resend
      // NAK, garbage or timeout: resend
    }
    if (!sent) {
      await link.write(new Uint8Array([CAN, CAN, CAN]));
      throw new Error("XMODEM: block " + (i + 1) + " was not acknowledged");
    }
    onProgress(i + 1, blocks.length);
  }
  for (let attempt = 0; attempt < 10; attempt++) {
    await link.write(new Uint8Array([EOT]));
    if ((await link.readByte(2000)) === ACK) return;
  }
  throw new Error("XMODEM: end of transfer was not acknowledged");
}
