// Click a Mermaid diagram to view it full screen.
//
// Mermaid scales a diagram down to the content column, which makes wide
// flowcharts unreadable. Material renders each diagram into a *closed* shadow
// root (so its SVG cannot be restyled from here); instead, the rendered host
// element itself is moved into a modal <dialog>, where the SVG grows up to its
// natural size, and is put back in place when the dialog closes.
(() => {
  const dialog = document.createElement("dialog");
  dialog.className = "ls-diagram-dialog";
  dialog.setAttribute("aria-label", "Diagram, full screen");

  const hint = document.createElement("p");
  hint.className = "ls-diagram-dialog__hint";
  hint.textContent = "Click or press Esc to close";
  dialog.append(hint);

  let shown = null;
  const placeholder = document.createComment("diagram shown full screen");

  dialog.addEventListener("close", () => {
    if (shown) placeholder.replaceWith(shown);
    shown = null;
  });
  dialog.addEventListener("click", () => dialog.close());

  document.addEventListener("click", (event) => {
    // Clicks inside the shadow root are retargeted to the host element.
    const host = event.target.closest?.(".md-typeset div.mermaid");
    if (!host || dialog.open) return;
    shown = host;
    host.replaceWith(placeholder);
    dialog.append(host);
    if (!dialog.isConnected) document.body.append(dialog);
    dialog.showModal();
  });
})();
