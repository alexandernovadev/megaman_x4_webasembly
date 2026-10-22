// Corre el juego fuera del hilo principal. El disco del usuario se monta con WORKERFS,
// que lee los sectores bajo demanda con FileReaderSync: no se carga ni se sube nada.
let ready = false;

var Module = {
  noInitialRun: true,
  print: (text) => postMessage({ type: 'log', text }),
  printErr: (text) => postMessage({ type: 'log', text }),
  onRuntimeInitialized: () => { ready = true; postMessage({ type: 'ready' }); },
  onAbort: (what) => postMessage({ type: 'error', text: String(what) }),
  onExit: (code) => postMessage({ type: 'exit', code }),
};

importScripts('x4.js');

onmessage = (e) => {
  const m = e.data;
  if (!ready) return;  // la página no deja empezar antes de 'ready'
  if (m.type === 'start') {
    const FS = Module.FS;
    FS.mkdir('/disc');
    FS.mount(Module.WORKERFS, { files: [m.file] }, '/disc');
    // Tarjeta de memoria guardada en el navegador (128 KB), o una nueva ya formateada.
    if (m.card && m.card.byteLength === 131072) Module.HEAPU8.set(new Uint8Array(m.card), Module._x4_card());
    else Module._x4_card_new();
    Module._x4_test_save(m.testSave ? 1 : 0);
    // HD: piezas que ya existen (pares de 32 bits), por tandas en el búfer del juego.
    if (m.hd) {
      const buf = Module._x4_hd_buf() >> 2, heap = new Uint32Array(Module.HEAPU8.buffer);
      for (let i = 0; i < m.hd.known.length; i += 131072) {
        const part = m.hd.known.subarray(i, i + 131072);
        heap.set(part, buf);
        Module._x4_hd_known(part.length >> 1);
      }
      Module._x4_hd_enable(m.hd.on ? 1 : 0, m.hd.capture ? 1 : 0);
    }
    Module.callMain(['/disc/' + m.file.name]);
  } else if (m.type === 'pad') {
    Module._x4_set_pad(m.buttons);
  } else if (m.type === 'credits') {
    Module._x4_add_credits(m.n);
  } else if (m.type === 'aa') {
    Module._x4_set_aa(m.on ? 1 : 0);
  } else if (m.type === 'wide') {
    Module._x4_set_widescreen(m.on ? 1 : 0);
  } else if (m.type === 'full') {
    Module._x4_full_toggle();
  } else if (m.type === 'pause') {
    Module._x4_set_paused(m.paused ? 1 : 0);
  }
};
