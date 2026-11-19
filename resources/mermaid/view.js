async function showDiagram(source) {
    const container = document.getElementById('diagram');
    mermaid.initialize({startOnLoad: false, securityLevel: 'strict', htmlLabels: false,
        secure: ['securityLevel', 'startOnLoad', 'maxTextSize', 'suppressErrorRendering', 'htmlLabels'],
        suppressErrorRendering: true});
    try {
        const drawing = await mermaid.render('ivyDiagram', source);
        container.innerHTML = drawing.svg;
    } catch (error) {
        container.textContent = String(error);
    }
}
