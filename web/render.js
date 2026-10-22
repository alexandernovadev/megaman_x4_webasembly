// Presentación de los frames con WebGL2 y filtros a la resolución real de la pantalla.
//   nitido:    sharp-bilinear (píxeles limpios con cualquier escala)
//   suavizado: xBR nivel 2 (Hyllian): bordes a 45°, 30° y 60° redondeados con antialias
//   scalefx:   ScaleFX (Sp00kyFox), 5 pasadas a 3x, ver scalefx.js; mientras carga, xBR
//   crt:       líneas de barrido, máscara de fósforo, halo, curvatura y viñeta
// Los vídeos (modo de 24 bits) tienen su propia cadena salvo en CRT: las redes de Anime4K
// (restauración de la compresión MDEC y superresolución 2x/4x, ver anime4k.js) y Lanczos hasta
// la pantalla. Mientras cargan, o sin texturas flotantes: desbloqueo, Lanczos y CAS.

// `flip` = 1 al dibujar en pantalla; las texturas intermedias guardan la imagen sin invertir.
const VERT = `#version 300 es
in vec2 p;
uniform float flip;
out vec2 uv;
void main() { uv = p * 0.5 + 0.5; if (flip > 0.5) uv.y = 1.0 - uv.y; gl_Position = vec4(p, 0.0, 1.0); }`;

const HEAD = `#version 300 es
precision highp float;
in vec2 uv;
uniform sampler2D tex;
uniform vec2 texSize, outSize;
out vec4 o;
vec3 px(ivec2 q) { return texelFetch(tex, clamp(q, ivec2(0), ivec2(texSize) - 1), 0).rgb; }
`;

const SHARP = HEAD + `
void main() {
  vec2 texel = uv * texSize;
  vec2 scale = max(floor(outSize / texSize), 1.0);
  vec2 s = fract(texel) - 0.5;
  vec2 range = 0.5 - 0.5 / scale;
  vec2 f = (s - clamp(s, -range, range)) * scale + 0.5;
  o = texture(tex, (floor(texel) + f) / texSize);
}`;

// xBR nivel 2. Para la esquina del píxel hacia la que cae el fragmento compara la fuerza de las
// dos diagonales en un entorno de 4x4 (distancias de color en YUV); si el borde va en diagonal
// rellena la esquina con el vecino más parecido, con pendiente de 45°, o de 30°/60° cuando el
// borde continúa en horizontal o vertical.
const XBR = HEAD + `
float d(vec3 a, vec3 b) {
  vec3 c = a - b;
  return 48.0 * abs(dot(c, vec3(0.299, 0.587, 0.114))) + 7.0 * abs(dot(c, vec3(-0.169, -0.331, 0.499)))
       + 6.0 * abs(dot(c, vec3(0.499, -0.418, -0.0813)));
}
bool ne(vec3 a, vec3 b) { return d(a, b) > 2.0; }
void main() {
  vec2 t = uv * texSize;
  ivec2 c = ivec2(floor(t));
  vec2 fp = fract(t);
  ivec2 dx = ivec2(fp.x < 0.5 ? -1 : 1, 0), dy = ivec2(0, fp.y < 0.5 ? -1 : 1);
  vec2 q = vec2(dx.x > 0 ? fp.x : 1.0 - fp.x, dy.y > 0 ? fp.y : 1.0 - fp.y);
  vec3 E = px(c), F = px(c + dx), H = px(c + dy), I = px(c + dx + dy);
  vec3 B = px(c - dy), D = px(c - dx), C = px(c + dx - dy), G = px(c - dx + dy);
  vec3 F4 = px(c + 2 * dx), I4 = px(c + 2 * dx + dy), H5 = px(c + 2 * dy), I5 = px(c + dx + 2 * dy);
  float wd1 = d(E, C) + d(E, G) + d(I, F4) + d(I, H5) + 4.0 * d(H, F);
  float wd2 = d(H, D) + d(H, I5) + d(F, I4) + d(F, B) + 4.0 * d(E, I);
  vec3 col = E;
  if (wd1 < wd2 && ne(E, F) && ne(E, H)) {
    bool shallow = 2.0 * d(F, G) <= d(H, C) && ne(E, G) && ne(D, G);
    bool steep = d(F, G) >= 2.0 * d(H, C) && ne(E, C) && ne(B, C);
    float s45 = q.x + q.y, s30 = q.y + 0.5 * q.x, s60 = q.x + 0.5 * q.y;
    float a = smoothstep(1.5 - fwidth(s45), 1.5 + fwidth(s45), s45);
    if (shallow) a = max(a, smoothstep(1.0 - fwidth(s30), 1.0 + fwidth(s30), s30));
    if (steep) a = max(a, smoothstep(1.0 - fwidth(s60), 1.0 + fwidth(s60), s60));
    col = mix(E, d(E, F) <= d(E, H) ? F : H, a);
  }
  o = vec4(col, 1.0);
}`;

// Desbloqueo: el vídeo MDEC se comprime en bloques de 8x8; en los bordes de bloque se suaviza
// el salto si es pequeño (artefacto), y se respeta si es grande (borde real de la imagen).
const DEBLOCK = HEAD + `
vec3 across(vec3 c, ivec2 q) {
  vec3 n = px(q);
  float diff = max(max(abs(c.r - n.r), abs(c.g - n.g)), abs(c.b - n.b));
  return mix(c, n, 0.4 * (1.0 - smoothstep(0.02, 0.09, diff)));
}
void main() {
  ivec2 p = ivec2(floor(uv * texSize));
  vec3 c = px(p);
  ivec2 m = p % 8;
  if (m.x == 7) c = across(c, p + ivec2(1, 0)); else if (m.x == 0) c = across(c, p - ivec2(1, 0));
  if (m.y == 7) c = across(c, p + ivec2(0, 1)); else if (m.y == 0) c = across(c, p - ivec2(0, 1));
  o = vec4(c, 1.0);
}`;

// Lanczos de 4x4 con antihalo: el resultado se limita al rango de los 4 vecinos más cercanos.
const LANCZOS = HEAD + `
float L(float x) {
  x = abs(x);
  if (x < 1e-4) return 1.0;
  if (x >= 2.0) return 0.0;
  float a = 3.14159265 * x;
  return 2.0 * sin(a) * sin(a * 0.5) / (a * a);
}
void main() {
  vec2 t = uv * texSize - 0.5;
  ivec2 b = ivec2(floor(t));
  vec2 f = t - floor(t);
  vec3 sum = vec3(0.0), lo = vec3(1.0), hi = vec3(0.0);
  float wsum = 0.0;
  for (int j = -1; j <= 2; j++) {
    float wy = L(float(j) - f.y);
    for (int i = -1; i <= 2; i++) {
      vec3 s = px(b + ivec2(i, j));
      float w = L(float(i) - f.x) * wy;
      sum += s * w;
      wsum += w;
      if (i >= 0 && i <= 1 && j >= 0 && j <= 1) { lo = min(lo, s); hi = max(hi, s); }
    }
  }
  o = vec4(clamp(sum / wsum, lo, hi), 1.0);
}`;

// CAS (AMD FidelityFX Contrast Adaptive Sharpening): enfoca más donde hay poco contraste y
// menos donde ya hay mucho, sin crear halos.
const CAS = HEAD + `
void main() {
  ivec2 p = ivec2(floor(uv * texSize));
  vec3 b = px(p + ivec2(0, -1)), d = px(p + ivec2(-1, 0)), e = px(p), f = px(p + ivec2(1, 0)), h = px(p + ivec2(0, 1));
  vec3 mn = min(min(min(b, d), min(f, h)), e), mx = max(max(max(b, d), max(f, h)), e);
  vec3 amp = sqrt(clamp(min(mn, 1.0 - mx) / max(mx, 1e-4), 0.0, 1.0));
  vec3 w = -amp / mix(8.0, 5.0, 0.6);
  o = vec4(clamp((b * w + d * w + f * w + h * w + e) / (1.0 + 4.0 * w), 0.0, 1.0), 1.0);
}`;

const CRT = `#version 300 es
precision highp float;
in vec2 uv;
uniform sampler2D tex;
uniform vec2 texSize, outSize;
out vec4 o;
vec2 curve(vec2 p) {
  p = p * 2.0 - 1.0;
  p *= 1.0 + vec2(p.y * p.y, p.x * p.x) * vec2(0.035, 0.045);
  return p * 0.5 + 0.5;
}
vec3 lin(vec3 c) { return pow(c, vec3(2.2)); }
void main() {
  vec2 p = curve(uv);
  if (p.x < 0.0 || p.y < 0.0 || p.x > 1.0 || p.y > 1.0) { o = vec4(0.0, 0.0, 0.0, 1.0); return; }
  // Modos entrelazados (480 líneas): las líneas de barrido siguen siendo 240.
  float lines = texSize.y > 300.0 ? texSize.y * 0.5 : texSize.y;
  float y = p.y * lines;
  float row = floor(y) + 0.5;
  // Haz horizontal algo nítido: mezcla de las dos columnas más cercanas.
  float x = p.x * texSize.x - 0.5;
  float fx = fract(x);
  fx = fx * fx * (3.0 - 2.0 * fx);
  vec2 a = vec2((floor(x) + 0.5) / texSize.x, row / lines);
  vec2 b = vec2((floor(x) + 1.5) / texSize.x, row / lines);
  vec3 col = mix(lin(texture(tex, a).rgb), lin(texture(tex, b).rgb), fx);
  // Halo: las zonas claras desbordan un poco sobre las líneas oscuras, como el fósforo real.
  vec3 glow = vec3(0.0);
  for (int j = -1; j <= 1; j++)
    for (int i = -1; i <= 1; i++)
      glow += lin(texture(tex, p + vec2(float(i), float(j)) * 1.5 / texSize).rgb);
  glow /= 9.0;
  // Perfil gaussiano de la línea, más ancho cuanto más brillante (como el fósforo real).
  float d = y - row;
  float bright = max(col.r, max(col.g, col.b));
  float w = mix(0.28, 0.42, bright);
  col *= exp(-d * d / (2.0 * w * w)) * 1.45;
  col += glow * 0.18;
  // Máscara de apertura RGB a la resolución de salida.
  int m = int(mod(gl_FragCoord.x, 3.0));
  vec3 mask = m == 0 ? vec3(1.0, 0.72, 0.72) : m == 1 ? vec3(0.72, 1.0, 0.72) : vec3(0.72, 0.72, 1.0);
  col *= mask * 1.18;
  // Viñeta suave.
  vec2 v = p * (1.0 - p);
  col *= pow(v.x * v.y * 16.0, 0.12);
  o = vec4(pow(col, vec3(1.0 / 2.2)), 1.0);
}`;

const FILTERS = { nitido: 'Nítido', suavizado: 'Suavizado (xBR)', scalefx: 'ScaleFX', crt: 'CRT (tele antigua)' };

function createRenderer(canvas) {
  const gl = canvas.getContext('webgl2', { antialias: false, alpha: false, preserveDrawingBuffer: true });
  if (!gl) return null;

  function program(fs) {
    const p = gl.createProgram();
    for (const [type, src] of [[gl.VERTEX_SHADER, VERT], [gl.FRAGMENT_SHADER, fs]]) {
      const s = gl.createShader(type);
      gl.shaderSource(s, src);
      gl.compileShader(s);
      if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(s));
      gl.attachShader(p, s);
    }
    gl.bindAttribLocation(p, 0, 'p');
    gl.linkProgram(p);
    return p;
  }
  const progs = {
    sharp: program(SHARP), xbr: program(XBR), crt: program(CRT),
    deblock: program(DEBLOCK), lanczos: program(LANCZOS), cas: program(CAS),
  };

  gl.bindBuffer(gl.ARRAY_BUFFER, gl.createBuffer());
  gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
  gl.enableVertexAttribArray(0);
  gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);

  let a4k = null, sfx = null;
  if (window.X4ScaleFX)
    X4ScaleFX.loadScaleFX(gl).then((f) => { sfx = f; draw(); }).catch((e) => console.warn('ScaleFX no disponible:', e.message));
  if (window.X4Anime4K)
    X4Anime4K.loadAnime4K(gl, () => gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4))
      .then((n) => { a4k = n; })
      .catch((e) => console.warn('Anime4K no disponible:', e.message));

  function texture(filter) {
    const t = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, t);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, filter);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, filter);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    return t;
  }
  const src = texture(gl.LINEAR);

  // Texturas intermedias, creadas o redimensionadas al pedirlas.
  const targets = {};
  function target(name, tw, th) {
    let t = targets[name];
    if (!t) t = targets[name] = { tex: texture(gl.LINEAR), fb: gl.createFramebuffer(), w: 0, h: 0 };
    if (t.w !== tw || t.h !== th) {
      t.w = tw; t.h = th;
      gl.bindTexture(gl.TEXTURE_2D, t.tex);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, tw, th, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
      gl.bindFramebuffer(gl.FRAMEBUFFER, t.fb);
      gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, t.tex, 0);
    }
    return t;
  }

  let w = 0, h = 0, last = null, video = false, wide = false, filter = 'nitido', enhanceVideo = true;
  // HD (hd.js): frames que llegan como lista de dibujo y se dibujan con la GPU a resolución de pantalla.
  let hd = null, lastHD = null;
  try { if (window.X4HD) hd = X4HD.create(gl); } catch (e) { console.warn('HD no disponible:', e.message); }

  function resize() {
    const r = canvas.getBoundingClientRect(), dpr = window.devicePixelRatio || 1;
    const cw = Math.max(1, Math.round(r.width * dpr)), ch = Math.max(1, Math.round(r.height * dpr));
    if (canvas.width !== cw || canvas.height !== ch) { canvas.width = cw; canvas.height = ch; }
  }

  // Rectángulo de la imagen dentro del lienzo: la PS1 muestra 4:3 sea cual sea la anchura en
  // píxeles; un frame panorámico es 4:3 ampliado en proporción a sus columnas extra.
  let view = { x: 0, y: 0, w: 1, h: 1 };
  function layout() {
    const W = canvas.width, H = canvas.height;
    const aspect = wide ? (4 / 3) * (w / 320) : 4 / 3;
    let vw = W, vh = Math.round(W / aspect);
    if (vh > H) { vh = H; vw = Math.round(H * aspect); }
    view = { x: (W - vw) >> 1, y: (H - vh) >> 1, w: vw, h: vh };
  }

  // Dibuja `prog` leyendo `tex` (tw x th) en `out` (null = pantalla, en `view`) de ow x oh.
  function pass(prog, tex, tw, th, out, ow, oh) {
    gl.useProgram(prog);
    gl.bindFramebuffer(gl.FRAMEBUFFER, out ? out.fb : null);
    if (out) gl.viewport(0, 0, ow, oh); else gl.viewport(view.x, view.y, ow, oh);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.uniform1i(gl.getUniformLocation(prog, 'tex'), 0);
    gl.uniform1f(gl.getUniformLocation(prog, 'flip'), out ? 0 : 1);
    gl.uniform2f(gl.getUniformLocation(prog, 'texSize'), tw, th);
    gl.uniform2f(gl.getUniformLocation(prog, 'outSize'), ow, oh);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
  }

  function draw() {
    if (!last && !lastHD) return;
    resize();
    layout();
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, canvas.width, canvas.height);
    gl.clearColor(0, 0, 0, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    if (lastHD) return hd.draw(lastHD, view);
    const W = view.w, H = view.h;
    if (filter === 'crt') return pass(progs.crt, src, w, h, null, W, H);
    if (video && enhanceVideo && a4k) {
      let m = a4k.upM(a4k.restore({ tex: src, w, h }));
      if (W > m.w * 1.2) m = a4k.upS(m);
      return pass(progs.lanczos, m.tex, m.w, m.h, null, W, H);
    }
    if (video && enhanceVideo) {
      const a = target('deblock', w, h), b = target('scaled', W, H);
      pass(progs.deblock, src, w, h, a, w, h);
      pass(progs.lanczos, a.tex, w, h, b, W, H);
      return pass(progs.cas, b.tex, W, H, null, W, H);
    }
    if (filter === 'scalefx' && sfx) {
      // ScaleFX lee el original píxel a píxel: sin interpolación mientras se ejecuta.
      gl.bindTexture(gl.TEXTURE_2D, src);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
      const m = sfx(src, w, h);
      gl.bindTexture(gl.TEXTURE_2D, src);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
      return pass(progs.sharp, m.tex, m.w, m.h, null, W, H);
    }
    if (filter === 'suavizado' || filter === 'scalefx') return pass(progs.xbr, src, w, h, null, W, H);
    pass(progs.sharp, src, w, h, null, W, H);
  }

  return {
    frame(buf, fw, fh, isVideo, isWide) {
      wide = !!isWide;
      gl.bindTexture(gl.TEXTURE_2D, src);
      if (fw !== w || fh !== h) { w = fw; h = fh; gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, w, h, 0, gl.RGBA, gl.UNSIGNED_BYTE, null); }
      gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array(buf));
      last = buf;
      lastHD = null;
      video = !!isVideo;
      draw();
    },
    hdFrame(m) {
      wide = m.margin > 0;
      w = m.w; h = m.h;
      lastHD = m;
      draw();
    },
    hd,
    setFilter(f) { filter = f in FILTERS ? f : 'nitido'; draw(); },
    get filter() { return filter; },
    setEnhanceVideo(on) { enhanceVideo = !!on; draw(); },
    redraw: draw,
    // Dónde está la imagen del juego en la página (px CSS) y su tamaño en píxeles de la PS1.
    geometry() {
      const s = canvas.getBoundingClientRect().width / Math.max(1, canvas.width);
      return { x: view.x * s, y: view.y * s, w: view.w * s, h: view.h * s, fw: w, fh: h,
               margin: wide ? (w - 320) / 2 : 0 };
    },
  };
}

window.X4Render = { FILTERS, createRenderer };
