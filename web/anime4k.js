// Ejecuta en WebGL2 shaders con el formato de "hooks" de mpv, en concreto los de Anime4K
// (bloc97, licencia MIT, copia en web/anime4k/). Cada bloque //!DESC es una pasada: lee las
// texturas de //!BIND, escribe la de //!SAVE con el tamaño de //!WIDTH/HEIGHT (expresiones en
// notación polaca inversa) y define vec4 hook(). MAIN es la imagen que entra y la que sale.
(function () {
  const VERT = `#version 300 es
in vec2 p;
out vec2 uv;
void main() { uv = p * 0.5 + 0.5; gl_Position = vec4(p, 0.0, 1.0); }`;

  function parse(text) {
    const passes = [];
    let cur = null;
    for (const line of text.split('\n')) {
      const m = line.match(/^\/\/!(\w+)\s*(.*)$/);
      if (m) {
        if (m[1] === 'DESC') { cur = { binds: [], save: 'MAIN', w: null, h: null, body: [] }; passes.push(cur); }
        else if (m[1] === 'BIND') cur.binds.push(m[2].trim());
        else if (m[1] === 'SAVE') cur.save = m[2].trim();
        else if (m[1] === 'WIDTH') cur.w = m[2].trim();
        else if (m[1] === 'HEIGHT') cur.h = m[2].trim();
        continue;
      }
      if (cur) cur.body.push(line);
    }
    return passes;
  }

  // "conv2d_tf.w 2 *" con los tamaños actuales de las texturas.
  function evalSize(expr, sizes, fallback) {
    if (!expr) return fallback;
    const st = [];
    for (const t of expr.split(/\s+/)) {
      if (/^[\d.]+$/.test(t)) st.push(parseFloat(t));
      else if (/^\w+\.[wh]$/.test(t)) { const [n, c] = t.split('.'); st.push(sizes[n][c === 'w' ? 0 : 1]); }
      else { const b = st.pop(), a = st.pop(); st.push(t === '+' ? a + b : t === '-' ? a - b : t === '*' ? a * b : a / b); }
    }
    return Math.round(st[0]);
  }

  function createChain(gl, text, quad) {
    const passes = parse(text);
    for (const ps of passes) {
      const pre = ps.binds.map((n) => `uniform sampler2D ${n}_raw; uniform vec2 ${n}_size;
#define ${n}_pos uv
#define ${n}_pt (1.0 / ${n}_size)
#define ${n}_mul 1.0
#define ${n}_tex(p) texture(${n}_raw, p)
#define ${n}_texOff(off) ${n}_tex(${n}_pos + ${n}_pt * (off))
`).join('');
      const fs = `#version 300 es
precision highp float;
in vec2 uv;
out vec4 fragOut;
${pre}
${ps.body.join('\n')}
void main() { fragOut = hook(); }`;
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
      ps.prog = p;
      ps.loc = ps.binds.map((n) => [gl.getUniformLocation(p, `${n}_raw`), gl.getUniformLocation(p, `${n}_size`)]);
    }

    // Texturas de salida en coma flotante (la red trabaja con valores negativos), una por pasada.
    const outs = passes.map(() => ({ tex: gl.createTexture(), fb: gl.createFramebuffer(), w: 0, h: 0 }));
    function ensure(o, w, h) {
      if (o.w === w && o.h === h) return;
      o.w = w; o.h = h;
      gl.bindTexture(gl.TEXTURE_2D, o.tex);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA16F, w, h, 0, gl.RGBA, gl.HALF_FLOAT, null);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
      gl.bindFramebuffer(gl.FRAMEBUFFER, o.fb);
      gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, o.tex, 0);
    }

    // Ejecuta la cadena sobre `main` ({tex, w, h}); devuelve el nuevo MAIN.
    return function run(main) {
      const tex = { MAIN: main.tex }, sizes = { MAIN: [main.w, main.h] };
      passes.forEach((ps, i) => {
        const w = evalSize(ps.w, sizes, sizes.MAIN[0]), h = evalSize(ps.h, sizes, sizes.MAIN[1]);
        const o = outs[i];
        ensure(o, w, h);
        gl.useProgram(ps.prog);
        gl.bindFramebuffer(gl.FRAMEBUFFER, o.fb);
        gl.viewport(0, 0, w, h);
        ps.binds.forEach((n, k) => {
          gl.activeTexture(gl.TEXTURE0 + k);
          gl.bindTexture(gl.TEXTURE_2D, tex[n]);
          gl.uniform1i(ps.loc[k][0], k);
          gl.uniform2f(ps.loc[k][1], sizes[n][0], sizes[n][1]);
        });
        quad();
        tex[ps.save] = o.tex;
        sizes[ps.save] = [w, h];
      });
      gl.activeTexture(gl.TEXTURE0);
      return { tex: tex.MAIN, w: sizes.MAIN[0], h: sizes.MAIN[1] };
    };
  }

  // Carga las tres redes: restauración (artefactos de compresión), 2x calidad media y 2x ligera.
  async function loadAnime4K(gl, quad) {
    if (!gl.getExtension('EXT_color_buffer_float')) throw new Error('sin texturas flotantes renderizables');
    const names = ['Anime4K_Restore_CNN_M', 'Anime4K_Upscale_CNN_x2_M', 'Anime4K_Upscale_CNN_x2_S'];
    const texts = await Promise.all(names.map((n) => fetch(`anime4k/${n}.glsl`).then((r) => r.text())));
    const [restore, upM, upS] = texts.map((t) => createChain(gl, t, quad));
    return { restore, upM, upS };
  }

  window.X4Anime4K = { loadAnime4K };
})();
