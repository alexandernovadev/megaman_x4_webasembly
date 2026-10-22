// Renderizador HD: vuelve a dibujar cada frame con la GPU, a la resolución de la pantalla, a
// partir de la lista de dibujo que manda el juego (runtime/hd.c). Cada pieza de textura que
// tiene versión HD (8x, hecha con IA, ver tools/hd/upscale.py) se dibuja con ella; las demás,
// con la textura original de la VRAM. Las piezas HD son índices de paleta: el color sale de la
// paleta actual del juego (VRAM), así que destellos y cambios de color siguen funcionando.
//
// Las piezas HD viven en el servidor local (tools/serve.py, carpeta hd_pack/) y se cargan bajo
// demanda en un atlas de la GPU (capas de 2048x2048 con 256 celdas de 128x128); si se llena,
// se descartan las menos usadas.
(function () {
  const VS = `#version 300 es
layout(location = 0) in vec2 pos;
layout(location = 1) in vec2 uv;
layout(location = 2) in uvec4 a;
uniform vec2 bufSize;
uniform float margin;
out vec2 vuv;
out vec3 vcol;
flat out uvec4 va;
void main() {
  vuv = uv;
  vcol = vec3(float(a.x & 255u), float((a.x >> 8) & 255u), float((a.x >> 16) & 255u));
  va = a;
  vec2 p = (pos + vec2(margin, 0.0)) / bufSize;
  gl_Position = vec4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
}`;

  const FS = `#version 300 es
precision highp float;
precision highp int;
precision highp usampler2D;
precision highp usampler2DArray;
uniform usampler2D vram, slots, pals, low;
uniform usampler2DArray atlas;
uniform int pass, mode;
in vec2 vuv;
in vec3 vcol;
flat in uvec4 va;
out vec4 o;

uint vr(int x, int y) { return texelFetch(vram, ivec2(x & 1023, y & 511), 0).r; }
vec3 c15(uint c) { return vec3(float(c & 31u), float((c >> 5) & 31u), float((c >> 10) & 31u)) / 31.0; }

// Color de la paleta pi del frame (la que había al dibujar la pieza).
uint palc(uint pi, uint idx) { return texelFetch(pals, ivec2(int(idx), int(pi)), 0).r; }

uint lowtex(ivec2 t, uint tex, uint twin, uint pi) {
  uint mx = twin & 31u, my = (twin >> 5) & 31u, ox = (twin >> 10) & 31u, oy = (twin >> 15) & 31u;
  int u = int((uint(t.x & 255) & ~(mx * 8u)) | ((ox & mx) * 8u)) & 255;
  int v = int((uint(t.y & 255) & ~(my * 8u)) | ((oy & my) * 8u)) & 255;
  int tpx = int(tex & 15u) * 64, tpy = int((tex >> 4) & 1u) * 256, depth = int((tex >> 5) & 3u);
  int y = tpy + v;
  if (depth == 0) { uint w = vr(tpx + u / 4, y); return palc(pi, (w >> uint((u & 3) * 4)) & 15u); }
  if (depth == 1) { uint w = vr(tpx + u / 2, y); return palc(pi, (w >> uint((u & 1) * 8)) & 255u); }
  return vr(tpx + u, y);
}

void main() {
  uint flags = va.x >> 24;
  bool textured = (flags & 1u) != 0u, raw = (flags & 2u) != 0u, semi = (flags & 4u) != 0u;
  vec3 rgb;
  float a = 1.0;
  bool semiFrag = semi;
  if (!textured) {
    rgb = vcol / 255.0;
  } else {
    uint pi = (va.y >> 7) & 32767u;
    uint id = va.z & 65535u, slot = 0u;
    vec2 org = vec2(float((va.z >> 16) & 255u), float(va.z >> 24));
    ivec2 rsz = ivec2(int((va.w >> 20) & 15u) + 1, int((va.w >> 24) & 15u) + 1);
    if (id != 0u) slot = texelFetch(slots, ivec2(int(id & 255u), int(id >> 8)), 0).r;
    if (slot != 0u) {
      // Pieza HD: interpolación bilineal de los colores ya resueltos con la paleta.
      slot -= 1u;
      int layer = int(slot >> 8), cell = int(slot & 255u);
      ivec2 base = ivec2(cell & 15, cell >> 4) * 128;
      ivec2 lim = rsz * 8 - 1;
      vec2 l = (vuv - org) * 8.0;
      vec2 fw = fwidth(l);
      // Si un píxel de pantalla cubre más de 2 texels HD, 4 muestras bilineales en vez de 1.
      int n = max(fw.x, fw.y) > 2.0 ? 2 : 1;
      vec3 acc = vec3(0.0);
      float aacc = 0.0, best = -1.0;
      uint bestc = 0u;
      for (int sy = 0; sy < n; sy++)
        for (int sx = 0; sx < n; sx++) {
          vec2 q = l - 0.5 + (n == 2 ? (vec2(float(sx), float(sy)) - 0.5) * 0.5 * fw : vec2(0.0));
          ivec2 i0 = ivec2(floor(q));
          vec2 f = q - floor(q);
          for (int k = 0; k < 4; k++) {
            ivec2 d = ivec2(k & 1, k >> 1);
            float w = (d.x == 1 ? f.x : 1.0 - f.x) * (d.y == 1 ? f.y : 1.0 - f.y);
            ivec2 t = clamp(i0 + d, ivec2(0), lim);
            uint idx = texelFetch(atlas, ivec3(base + t, layer), 0).r;
            uint c = palc(pi, idx);
            if (w > best) { best = w; bestc = c; }
            if (c != 0u) { acc += c15(c) * w; aacc += w; }
          }
        }
      float tot = float(n * n);
      a = aacc / tot;
      if (a < 0.02) discard;
      rgb = acc / aacc;
      semiFrag = semi && (bestc & 0x8000u) != 0u;
    } else if (id != 0u) {
      // Pieza sin versión HD (todavía): sus texels tal como estaban al dibujarla, suavizados
      // (bilineal con transición en S) para que no se vea a cuadros junto a las piezas HD
      // mientras la IA la convierte.
      ivec2 cell = ivec2(int(id & 255u), int(id >> 8)) * 16;
      vec2 q = vuv - org - 0.5;
      ivec2 i0 = ivec2(floor(q));
      vec2 f = smoothstep(0.15, 0.85, q - floor(q));
      vec3 acc = vec3(0.0);
      float aacc = 0.0, best = -1.0;
      uint bestc = 0u;
      for (int k = 0; k < 4; k++) {
        ivec2 d = ivec2(k & 1, k >> 1);
        float w = (d.x == 1 ? f.x : 1.0 - f.x) * (d.y == 1 ? f.y : 1.0 - f.y);
        ivec2 t = clamp(i0 + d, ivec2(0), rsz - 1);
        uint c = palc(pi, texelFetch(low, cell + t, 0).r);
        if (w > best) { best = w; bestc = c; }
        if (c != 0u) { acc += c15(c) * w; aacc += w; }
      }
      a = aacc;
      if (a < 0.5) discard;
      a = 1.0;
      rgb = acc / aacc;
      semiFrag = semi && (bestc & 0x8000u) != 0u;
    } else {
      uint c = lowtex(ivec2(floor(vuv)), va.y, va.w, pi);
      if (c == 0u) discard;
      rgb = c15(c);
      semiFrag = semi && (c & 0x8000u) != 0u;
    }
    if (!raw) rgb = min(rgb * vcol / 128.0, 1.0);
  }
  if (pass == 1 && semiFrag) discard;
  if (pass == 2 && !semiFrag) discard;
  if (pass != 2) { o = vec4(rgb * a, a); return; }
  float k = mode == 0 ? 0.5 : mode == 3 ? 0.25 : 1.0;
  o = vec4(rgb * a * k, mode == 0 ? a * 0.5 : 0.0);
}`;

  const CELL = 128, PER_LAYER = 256;

  function create(gl) {
    const prog = gl.createProgram();
    for (const [type, src] of [[gl.VERTEX_SHADER, VS], [gl.FRAGMENT_SHADER, FS]]) {
      const s = gl.createShader(type);
      gl.shaderSource(s, src);
      gl.compileShader(s);
      if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(s));
      gl.attachShader(prog, s);
    }
    gl.linkProgram(prog);
    if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(prog));
    const U = {};
    for (const n of ['vram', 'slots', 'pals', 'low', 'atlas', 'pass', 'mode', 'bufSize', 'margin']) U[n] = gl.getUniformLocation(prog, n);

    const vao = gl.createVertexArray();
    const vbo = gl.createBuffer();
    gl.bindVertexArray(vao);
    gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 32, 0);
    gl.enableVertexAttribArray(1);
    gl.vertexAttribPointer(1, 2, gl.FLOAT, false, 32, 8);
    gl.enableVertexAttribArray(2);
    gl.vertexAttribIPointer(2, 4, gl.UNSIGNED_INT, 32, 16);
    gl.bindVertexArray(null);

    function itex(target) {
      const t = gl.createTexture();
      gl.bindTexture(target, t);
      gl.texParameteri(target, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
      gl.texParameteri(target, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
      gl.texParameteri(target, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
      gl.texParameteri(target, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
      return t;
    }
    const vramTex = itex(gl.TEXTURE_2D);
    gl.texStorage2D(gl.TEXTURE_2D, 1, gl.R16UI, 1024, 512);
    const slotTex = itex(gl.TEXTURE_2D);
    gl.texStorage2D(gl.TEXTURE_2D, 1, gl.R32UI, 256, 256);
    const slotData = new Uint32Array(65536);
    // Paletas del frame (una fila de 256 colores por paleta) y texels originales de cada pieza
    // (celdas de 16x16 por id), ambos tal como estaban al dibujar.
    const palTex = itex(gl.TEXTURE_2D);
    gl.texStorage2D(gl.TEXTURE_2D, 1, gl.R16UI, 256, 2048);
    const lowTex = itex(gl.TEXTURE_2D);
    gl.texStorage2D(gl.TEXTURE_2D, 1, gl.R8UI, 4096, 4096);
    let slotDirty = true;
    // Atlas grande (hasta 64 capas = 16384 piezas, 256 MB) para tener precargado todo el paquete;
    // si la GPU no puede, uno más pequeño.
    let layers = Math.min(64, gl.getParameter(gl.MAX_ARRAY_TEXTURE_LAYERS)), atlas;
    for (;;) {
      atlas = itex(gl.TEXTURE_2D_ARRAY);
      gl.texStorage3D(gl.TEXTURE_2D_ARRAY, 1, gl.R8UI, CELL * 16, CELL * 16, layers);
      if (gl.getError() === gl.NO_ERROR || layers <= 8) break;
      gl.deleteTexture(atlas);
      layers >>= 1;
    }
    const NS = layers * PER_LAYER;
    const slotHash = new Array(NS).fill(null), slotLast = new Float64Array(NS);
    const free = [];
    for (let i = NS - 1; i >= 0; i--) free.push(i);

    // ---------------------------------------------------------- piezas disponibles y carga
    // Todo lo que hay en el paquete se precarga en la GPU en cuanto se conoce (al empezar y
    // cada vez que la IA termina una pieza), sin esperar a que aparezca en pantalla: así no se
    // ve la pieza original mientras llega la HD. Las piezas viven en el atlas por huella; los ids
    // del juego apuntan a ellas al aparecer.
    const idHash = [];             // id -> huella (hex)
    const hashId = new Map();      // huella -> id
    const available = new Set();   // huellas que hay en el servidor
    const hashSlot = new Map();    // huella -> celda del atlas
    const loading = new Set();     // huellas pedidas
    let urgent = [], queue = [], inflight = 0, listed = 0, serverOk = false, frameNo = 0;
    const hex8 = (n) => (n >>> 0).toString(16).padStart(8, '0');

    async function poll() {
      try {
        const r = await fetch('hd/list?since=' + listed, { cache: 'no-store' });
        if (!r.ok) return false;
        const lines = (await r.text()).split('\n');
        const total = +lines[0];
        for (let i = 1; i < lines.length; i++) {
          const h = lines[i];
          if (!h || available.has(h)) continue;
          available.add(h);
          // Las que ya usa el juego, primero; las demás, mientras quepan.
          if (hashId.has(h)) urgent.push(h);
          else if (hashSlot.size + loading.size + queue.length < NS - 512) queue.push(h);
        }
        listed = total;
        pump();
        return true;
      } catch { return false; }
    }
    const ready = poll().then((ok) => {
      serverOk = ok;
      if (ok) setInterval(poll, 2000);
      return ok;
    });

    function alloc() {
      if (free.length) return free.pop();
      let best = -1, t = Infinity;
      for (let i = 0; i < NS; i++) if (slotLast[i] < t && slotLast[i] < frameNo) { t = slotLast[i]; best = i; }
      if (best < 0) return -1;
      const old = slotHash[best];
      if (old) {
        hashSlot.delete(old);
        const id = hashId.get(old);
        if (id) { slotData[id] = 0; slotDirty = true; }
      }
      slotHash[best] = null;
      return best;
    }

    function upload(hash, w, h, idx) {
      if (hashSlot.has(hash)) return;
      const s = alloc();
      if (s < 0) return;
      const layer = s >> 8, cell = s & 255;
      gl.bindTexture(gl.TEXTURE_2D_ARRAY, atlas);
      gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
      gl.texSubImage3D(gl.TEXTURE_2D_ARRAY, 0, (cell & 15) * CELL, (cell >> 4) * CELL, layer, w, h, 1,
        gl.RED_INTEGER, gl.UNSIGNED_BYTE, idx);
      slotHash[s] = hash; slotLast[s] = 0;
      hashSlot.set(hash, s);
      const id = hashId.get(hash);
      if (id) { slotData[id] = s + 1; slotDirty = true; slotLast[s] = frameNo; }
    }

    async function fetchBatch(hashes) {
      inflight++;
      try {
        const r = await fetch('hd/batch', { method: 'POST', body: hashes.join('\n') });
        const buf = new Uint8Array(await r.arrayBuffer()), dv = new DataView(buf.buffer);
        for (let o = 0; o + 12 <= buf.length;) {
          const hash = hex8(dv.getUint32(o + 4, true)) + hex8(dv.getUint32(o, true));
          const len = dv.getUint32(o + 8, true);
          const w = dv.getUint16(o + 16, true), h = dv.getUint16(o + 18, true);
          if (w <= CELL && h <= CELL) upload(hash, w, h, buf.subarray(o + 20, o + 20 + w * h));
          o += 12 + len;
        }
      } catch {}
      for (const h of hashes) loading.delete(h);
      inflight--;
      pump();
    }
    function pump() {
      while ((urgent.length || queue.length) && inflight < 4) {
        const src = urgent.length ? urgent : queue;
        const batch = [];
        while (src.length && batch.length < 256) {
          const h = src.shift();
          if (!hashSlot.has(h) && !loading.has(h)) { loading.add(h); batch.push(h); }
        }
        if (batch.length) fetchBatch(batch);
      }
    }

    // ---------------------------------------------------------- frame
    // Se dibuja con antialiasing (4 muestras) en `fbo`, se resuelve en `res` y se copia al lienzo.
    let fbo = null, rb = null, res = null, resTex = null, fw = 0, fh = 0;
    function target(w, h) {
      if (fbo && fw === w && fh === h) return;
      fw = w; fh = h;
      if (!fbo) { fbo = gl.createFramebuffer(); rb = gl.createRenderbuffer(); res = gl.createFramebuffer(); }
      gl.bindRenderbuffer(gl.RENDERBUFFER, rb);
      gl.renderbufferStorageMultisample(gl.RENDERBUFFER, Math.min(4, gl.getParameter(gl.MAX_SAMPLES)), gl.RGBA8, w, h);
      gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);
      gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.RENDERBUFFER, rb);
      if (resTex) gl.deleteTexture(resTex);
      resTex = gl.createTexture();
      gl.bindTexture(gl.TEXTURE_2D, resTex);
      gl.texStorage2D(gl.TEXTURE_2D, 1, gl.RGBA8, w, h);
      gl.bindFramebuffer(gl.FRAMEBUFFER, res);
      gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, resTex, 0);
    }

    let lastVram = null;
    function ingest(m) {
      // Ids nuevos que asignó el juego y uso de piezas de este frame.
      frameNo++;
      const ids = new Uint32Array(m.ids), idpx = new Uint8Array(m.idpx);
      gl.bindTexture(gl.TEXTURE_2D, lowTex);
      gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
      for (let i = 0, k = 0; i + 2 < ids.length; i += 3, k++) {
        const id = ids[i], hash = hex8(ids[i + 2]) + hex8(ids[i + 1]);
        idHash[id] = hash; hashId.set(hash, id);
        const ps = hashSlot.get(hash);
        if (ps !== undefined) { slotData[id] = ps + 1; slotDirty = true; }
        gl.texSubImage2D(gl.TEXTURE_2D, 0, (id & 255) * 16, (id >> 8) * 16, 16, 16, gl.RED_INTEGER, gl.UNSIGNED_BYTE,
          idpx.subarray(k * 256, k * 256 + 256));
      }
      const v = new Uint32Array(m.verts);
      for (let i = 6; i < v.length; i += 24) {  // un vértice de cada triángulo basta
        const id = v[i] & 0xffff;
        if (!id) continue;
        const s = slotData[id];
        if (s) { slotLast[s - 1] = frameNo; continue; }
        const h = idHash[id];
        if (available.has(h) && !loading.has(h)) urgent.push(h);
      }
      pump();
      lastVram = m.vram;
    }

    function draw(m, view) {
      if (m.ingested !== true) { ingest(m); m.ingested = true; }
      const W = view.w, H = view.h;
      target(W, H);
      gl.bindTexture(gl.TEXTURE_2D, vramTex);
      gl.pixelStorei(gl.UNPACK_ALIGNMENT, 2);
      gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, 1024, 512, gl.RED_INTEGER, gl.UNSIGNED_SHORT, new Uint16Array(m.vram));
      if (m.np) {
        gl.bindTexture(gl.TEXTURE_2D, palTex);
        gl.pixelStorei(gl.UNPACK_ALIGNMENT, 2);
        gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, 256, m.np, gl.RED_INTEGER, gl.UNSIGNED_SHORT, new Uint16Array(m.pals));
      }
      if (slotDirty) {
        gl.bindTexture(gl.TEXTURE_2D, slotTex);
        gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);
        gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, 256, 256, gl.RED_INTEGER, gl.UNSIGNED_INT, slotData);
        slotDirty = false;
      }
      gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);
      gl.viewport(0, 0, W, H);
      gl.disable(gl.SCISSOR_TEST);
      gl.clearColor(0, 0, 0, 1);
      gl.clear(gl.COLOR_BUFFER_BIT);
      gl.useProgram(prog);
      gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D, vramTex);
      gl.activeTexture(gl.TEXTURE1); gl.bindTexture(gl.TEXTURE_2D, slotTex);
      gl.activeTexture(gl.TEXTURE2); gl.bindTexture(gl.TEXTURE_2D_ARRAY, atlas);
      gl.activeTexture(gl.TEXTURE3); gl.bindTexture(gl.TEXTURE_2D, palTex);
      gl.activeTexture(gl.TEXTURE4); gl.bindTexture(gl.TEXTURE_2D, lowTex);
      gl.activeTexture(gl.TEXTURE0);
      gl.uniform1i(U.vram, 0); gl.uniform1i(U.slots, 1); gl.uniform1i(U.atlas, 2);
      gl.uniform1i(U.pals, 3); gl.uniform1i(U.low, 4);
      gl.uniform2f(U.bufSize, m.w, m.h);
      gl.uniform1f(U.margin, m.margin);
      gl.bindVertexArray(vao);
      gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
      gl.bufferData(gl.ARRAY_BUFFER, m.verts, gl.STREAM_DRAW);
      gl.enable(gl.BLEND);
      gl.enable(gl.SCISSOR_TEST);
      const b = new Int32Array(m.batches), sx = W / m.w, sy = H / m.h;
      for (let i = 0; i + 6 < b.length; i += 7) {
        const first = b[i], count = b[i + 1], blend = b[i + 2];
        const x0 = Math.round((b[i + 3] + m.margin) * sx), x1 = Math.round((b[i + 5] + 1 + m.margin) * sx);
        const y0 = Math.round(b[i + 4] * sy), y1 = Math.round((b[i + 6] + 1) * sy);
        gl.scissor(x0, H - y1, Math.max(0, x1 - x0), Math.max(0, y1 - y0));
        gl.blendEquation(gl.FUNC_ADD);
        gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
        gl.uniform1i(U.pass, blend ? 1 : 0);
        gl.drawArrays(gl.TRIANGLES, first, count);
        if (!blend) continue;
        const mode = blend - 1;
        gl.uniform1i(U.pass, 2);
        gl.uniform1i(U.mode, mode);
        if (mode === 0) gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
        else gl.blendFunc(gl.ONE, gl.ONE);
        if (mode === 2) gl.blendEquation(gl.FUNC_REVERSE_SUBTRACT);
        gl.drawArrays(gl.TRIANGLES, first, count);
      }
      gl.blendEquation(gl.FUNC_ADD);
      gl.disable(gl.BLEND);
      gl.disable(gl.SCISSOR_TEST);
      gl.bindVertexArray(null);
      // Resolver el antialiasing y copiar a su sitio en el lienzo.
      gl.bindFramebuffer(gl.READ_FRAMEBUFFER, fbo);
      gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, res);
      gl.blitFramebuffer(0, 0, W, H, 0, 0, W, H, gl.COLOR_BUFFER_BIT, gl.NEAREST);
      gl.bindFramebuffer(gl.READ_FRAMEBUFFER, res);
      gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, null);
      gl.blitFramebuffer(0, 0, W, H, view.x, view.y, view.x + W, view.y + H, gl.COLOR_BUFFER_BIT, gl.NEAREST);
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    }

    // Configuración para el juego: si hay servidor HD, qué piezas ya existen (para no volver a
    // capturarlas) como pares de enteros de 32 bits (parte baja, parte alta).
    async function config() {
      const ok = await ready;
      const known = new Uint32Array(available.size * 2);
      let i = 0;
      for (const h of available) { known[i++] = parseInt(h.slice(8), 16); known[i++] = parseInt(h.slice(0, 8), 16); }
      return { on: true, capture: ok, known };
    }

    return {
      draw, config,
      stats: () => ({ available: available.size, loaded: hashSlot.size, loading: loading.size, capacity: NS, server: serverOk }),
    };
  }

  window.X4HD = { create };
})();
