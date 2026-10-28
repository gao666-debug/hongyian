// Development-only TCP relay; TLS stays end-to-end with the WSL backend.
const net = require('node:net');
const host = process.argv[2];
if (!host) throw Error('Pass the current WSL IP');
net.createServer(client => {
  const upstream = net.connect(8443, host);
  client.pipe(upstream); upstream.pipe(client);
  client.on('error', () => upstream.destroy());
  upstream.on('error', () => client.destroy());
  client.on('close', () => upstream.destroy());
  upstream.on('close', () => client.destroy());
}).listen(18443, '127.0.0.1', () => console.log('Emulator relay listening on localhost:18443'));
