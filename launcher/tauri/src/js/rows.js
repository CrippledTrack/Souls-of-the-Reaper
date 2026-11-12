// @ts-check
// One menu row: a label, an optional tooltip and an optional value with ‹ ›
// arrows. Arrow keys cycle the value; Space and Enter activate the row.

/**
 * @typedef {object} RowOptions
 * @property {string} label
 * @property {string} [tip]
 * @property {string | {text: string, cls?: string}} [value]
 * @property {() => void} [onLeft]
 * @property {() => void} [onRight]
 * @property {() => void} [onClick]
 * @property {string} [cls]
 */

/**
 * @param {Document} doc
 * @param {RowOptions} o
 */
export function makeRow(doc, o) {
  const row = doc.createElement('div');
  row.className = 'opt-row' + (o.cls ? ' ' + o.cls : '');
  row.tabIndex = 0;
  let inner = '<span class="frame-band"></span><span class="opt-label">' + o.label + '</span>';
  if (o.tip) inner += '<span class="tip">' + o.tip + '</span>';
  if (o.value !== undefined) {
    const text = typeof o.value === 'object' ? o.value.text : o.value;
    const vcls = typeof o.value === 'object' && o.value.cls ? ' ' + o.value.cls : '';
    if (o.onLeft || o.onRight) {
      inner += '<div class="cycler">' +
        (o.onLeft ? '<button class="arw" data-a="l" tabindex="-1" aria-label="previous">‹</button>' : '') +
        '<span class="opt-val' + vcls + '">' + text + '</span>' +
        (o.onRight ? '<button class="arw" data-a="r" tabindex="-1" aria-label="next">›</button>' : '') +
        '</div>';
    } else {
      inner += '<span class="opt-val' + vcls + '">' + text + '</span>';
    }
  }
  row.innerHTML = inner;
  const l = row.querySelector('[data-a="l"]'), r = row.querySelector('[data-a="r"]');
  if (l) l.addEventListener('click', e => { e.stopPropagation(); o.onLeft?.(); });
  if (r) r.addEventListener('click', e => { e.stopPropagation(); o.onRight?.(); });
  if (o.onClick) row.addEventListener('click', o.onClick);
  row.addEventListener('keydown', e => {
    if (e.key === 'ArrowLeft' && o.onLeft) { e.preventDefault(); o.onLeft(); }
    else if (e.key === 'ArrowRight' && o.onRight) { e.preventDefault(); o.onRight(); }
    else if ((e.key === ' ' || e.key === 'Enter') && o.onClick) { e.preventDefault(); o.onClick(); }
  });
  return row;
}
