// Pixel text in the launcher's 5x7 font (font5x7.js), drawn as crisp SVG.
//
//   <span class="px" data-scale="3" data-look="arcade" data-color="#ffd93d">GAMES</span>
//
// data-scale  screen pixels per font pixel (default 2)
// data-look   plain | shadow (hard black drop shadow) | arcade (launcher titles:
//             coloured fill, white outline, black ring, hard black shadow)
// data-color  fill colour (default: the element's CSS colour)
// The text stays readable to screen readers (aria-label) and search.
"use strict";

function pixelSVG(text, { scale = 2, look = "plain", color = "currentColor" } = {}) {
	const s = scale, adv = 6 * s;
	// outline widths in screen pixels, as text_arcade() draws them
	const ol = look === "arcade" ? Math.max(1, Math.round(s / 3)) : 0;	// white
	const ring = look === "arcade" ? Math.max(1, Math.round(s / 3)) : 0;	// black
	const sh = look === "plain" ? 0 : Math.max(1, Math.round(s * 0.6));	// shadow offset
	const pad = ol + ring;
	const cells = [];
	[...text].forEach((ch, i) => {
		const g = FONT5X7[ch];
		if (!g) return;
		for (let y = 0; y < 7; y++)
			for (let x = 0; x < 5; x++)
				if (g[y] >> (4 - x) & 1) cells.push([i * adv + x * s, y * s]);
	});
	const w = Math.max(0, text.length * adv - s) + 2 * pad + sh, h = 7 * s + 2 * pad + sh;
	const layer = (grow, dx, dy, fill) =>
		`<g fill="${fill}">` + cells.map(([x, y]) =>
			`<rect x="${x + pad - grow + dx}" y="${y + pad - grow + dy}" width="${s + 2 * grow}" height="${s + 2 * grow}"/>`).join("") + `</g>`;
	let body = "";
	if (sh) body += layer(ol + ring, sh, sh, "#000");
	if (ring) body += layer(ol + ring, 0, 0, "#000");
	if (ol) body += layer(ol, 0, 0, "#fff");
	body += layer(0, 0, 0, color);
	return `<svg xmlns="http://www.w3.org/2000/svg" width="${w}" height="${h}" viewBox="0 0 ${w} ${h}" ` +
		`shape-rendering="crispEdges" aria-hidden="true" style="display:inline-block;vertical-align:middle;overflow:visible">${body}</svg>`;
}

// Render every .px element under root (call again after changing content).
function renderPixelText(root = document) {
	root.querySelectorAll(".px:not([data-px-done])").forEach(el => {
		const text = el.dataset.text || el.textContent;
		el.dataset.text = text;
		el.setAttribute("aria-label", text);
		el.innerHTML = pixelSVG(text, {
			scale: +(el.dataset.scale || 2),
			look: el.dataset.look || "plain",
			color: el.dataset.color || getComputedStyle(el).color,
		});
		el.dataset.pxDone = "1";
	});
}
