/**
 * Local AI Web UI - Frontend Application
 */

document.addEventListener('DOMContentLoaded', () => {
    // State
    const state = {
        messages: [],
        isGenerating: false,
        abortController: null,
        serverOnline: false,
        nCtx: 4096,
        usedCtx: 0,
        modelName: '',
        systemPrompt: '',
        temperature: 0.7
    };

    // DOM Elements
    const elements = {
        messagesContainer: document.getElementById('messages-container'),
        welcomeScreen: document.getElementById('welcome-screen'),
        chatForm: document.getElementById('chat-form'),
        userInput: document.getElementById('user-input'),
        btnSendMsg: document.getElementById('btn-send-msg'),
        btnStopGen: document.getElementById('btn-stop-gen'),
        newChatBtn: document.getElementById('new-chat-btn'),
        btnClearChat: document.getElementById('btn-clear-chat'),
        btnResetContext: document.getElementById('btn-reset-context'),
        btnRefreshStatus: document.getElementById('btn-refresh-status'),
        serverStatusDot: document.getElementById('server-status-dot'),
        serverStatusText: document.getElementById('server-status-text'),
        statusModelName: document.getElementById('status-model-name'),
        statusServerTarget: document.getElementById('status-server-target'),
        contextPct: document.getElementById('context-pct'),
        contextBarFill: document.getElementById('context-bar-fill'),
        contextUsedTokens: document.getElementById('context-used-tokens'),
        contextMaxTokens: document.getElementById('context-max-tokens'),
        tempSlider: document.getElementById('temp-slider'),
        tempVal: document.getElementById('temp-val'),
        systemPromptInput: document.getElementById('system-prompt-input'),
        sidebar: document.getElementById('sidebar'),
        mobileMenuToggle: document.getElementById('mobile-menu-toggle'),
        serverModal: document.getElementById('server-modal'),
        btnOpenServerModal: document.getElementById('btn-open-server-modal'),
        btnCloseModal: document.getElementById('btn-close-modal'),
        btnCancelModal: document.getElementById('btn-cancel-modal'),
        btnSaveModal: document.getElementById('btn-save-modal'),
        cfgServerHost: document.getElementById('cfg-server-host'),
        cfgServerPort: document.getElementById('cfg-server-port')
    };

    // --- Helper Functions ---

    function escapeHtml(text) {
        const div = document.createElement('div');
        div.textContent = text;
        return div.innerHTML;
    }

    function renderMarkdown(rawText) {
        if (!rawText) return '';

        // Extract code blocks first to protect them
        const codeBlocks = [];
        let text = rawText.replace(/```([a-zA-Z0-9_-]*)\n([\s\S]*?)```/g, (match, lang, code) => {
            const id = `__CODE_BLOCK_${codeBlocks.length}__`;
            codeBlocks.push({ lang: lang || 'text', code: code.trim() });
            return id;
        });

        // Inline formatting
        text = escapeHtml(text);

        // Bold
        text = text.replace(/\*\*(.*?)\*\*/g, '<strong>$1</strong>');
        // Italic
        text = text.replace(/\*(.*?)\*/g, '<em>$1</em>');
        // Inline code
        text = text.replace(/`([^`]+)`/g, '<code>$1</code>');

        // Headers
        text = text.replace(/^### (.*$)/gim, '<h3>$1</h3>');
        text = text.replace(/^## (.*$)/gim, '<h2>$1</h2>');
        text = text.replace(/^# (.*$)/gim, '<h1>$1</h1>');

        // Paragraphs and Line Breaks
        const lines = text.split('\n');
        let html = '';
        let inParagraph = false;

        for (let line of lines) {
            line = line.trim();
            if (line.startsWith('__CODE_BLOCK_') && line.endsWith('__')) {
                if (inParagraph) {
                    html += '</p>';
                    inParagraph = false;
                }
                const index = parseInt(line.replace('__CODE_BLOCK_', '').replace('__', ''), 10);
                const block = codeBlocks[index];
                if (block) {
                    html += `<div class="code-block-wrapper">
                        <div class="code-header">
                            <span>${escapeHtml(block.lang)}</span>
                            <button class="btn-copy-code" onclick="copyToClipboard(this)">Kopiera</button>
                        </div>
                        <pre><code>${escapeHtml(block.code)}</code></pre>
                    </div>`;
                }
            } else if (line === '') {
                if (inParagraph) {
                    html += '</p>';
                    inParagraph = false;
                }
            } else {
                if (!inParagraph) {
                    html += '<p>';
                    inParagraph = true;
                } else {
                    html += '<br>';
                }
                html += line;
            }
        }

        if (inParagraph) {
            html += '</p>';
        }

        return html;
    }

    window.copyToClipboard = function(btn) {
        const wrapper = btn.closest('.code-block-wrapper');
        const code = wrapper ? wrapper.querySelector('pre code').innerText : '';
        navigator.clipboard.writeText(code).then(() => {
            const originalText = btn.innerText;
            btn.innerText = 'Kopierat!';
            setTimeout(() => {
                btn.innerText = originalText;
            }, 2000);
        });
    };

    function updateContextMeter(used, max) {
        state.usedCtx = used;
        state.nCtx = max || 4096;

        const pct = state.nCtx > 0 ? ((state.usedCtx / state.nCtx) * 100).toFixed(1) : 0;
        elements.contextPct.textContent = `${pct}%`;
        elements.contextUsedTokens.textContent = state.usedCtx;
        elements.contextMaxTokens.textContent = state.nCtx;
        elements.contextBarFill.style.width = `${Math.min(pct, 100)}%`;

        elements.contextBarFill.classList.remove('warning', 'danger');
        if (pct >= 85) {
            elements.contextBarFill.classList.add('danger');
        } else if (pct >= 60) {
            elements.contextBarFill.classList.add('warning');
        }
    }

    async function checkServerStatus() {
        try {
            const res = await fetch('/api/status');
            const data = await res.json();

            if (data.status === 'ok' || data.type === 'pong') {
                state.serverOnline = data.status !== 'offline';
                state.modelName = data.model || 'Okänd modell';
                const serverTarget = `${data.target_host || '127.0.0.1'}:${data.target_port || 8080}`;
                elements.statusServerTarget.textContent = serverTarget;

                if (state.serverOnline) {
                    elements.serverStatusDot.className = 'status-dot online';
                    elements.serverStatusText.textContent = 'Ansluten';
                    elements.statusModelName.textContent = state.modelName.split('/').pop() || state.modelName;
                    elements.statusModelName.title = state.modelName;
                    updateContextMeter(data.used_ctx || 0, data.n_ctx || 4096);
                } else {
                    elements.serverStatusDot.className = 'status-dot offline';
                    elements.serverStatusText.textContent = 'Offline (Ingen server)';
                    elements.statusModelName.textContent = 'Ej ansluten';
                }
            } else {
                elements.serverStatusDot.className = 'status-dot offline';
                elements.serverStatusText.textContent = 'Offline';
            }
        } catch (err) {
            elements.serverStatusDot.className = 'status-dot offline';
            elements.serverStatusText.textContent = 'Offline';
            state.serverOnline = false;
        }
    }

    function scrollToBottom() {
        elements.messagesContainer.scrollTop = elements.messagesContainer.scrollHeight;
    }

    function appendMessage(role, content) {
        if (elements.welcomeScreen) {
            elements.welcomeScreen.remove();
        }

        const row = document.createElement('div');
        row.className = `message-row ${role}`;

        const avatar = document.createElement('div');
        avatar.className = 'message-avatar';
        avatar.textContent = role === 'user' ? '👤' : '🤖';

        const contentWrapper = document.createElement('div');
        contentWrapper.className = 'message-content-wrapper';

        const author = document.createElement('div');
        author.className = 'message-author';
        author.textContent = role === 'user' ? 'Du' : 'AI Assistent';

        const bubble = document.createElement('div');
        bubble.className = 'message-bubble';
        if (role === 'user') {
            bubble.textContent = content;
        } else {
            bubble.innerHTML = renderMarkdown(content);
        }

        contentWrapper.appendChild(author);
        contentWrapper.appendChild(bubble);

        row.appendChild(avatar);
        row.appendChild(contentWrapper);

        elements.messagesContainer.appendChild(row);
        scrollToBottom();

        return bubble;
    }

    async function handleSendMessage(text) {
        if (!text.trim() || state.isGenerating) return;

        const userMsg = text.trim();
        elements.userInput.value = '';
        elements.userInput.style.height = 'auto';

        // Add user message to state and UI
        state.messages.push({ role: 'user', content: userMsg });
        appendMessage('user', userMsg);

        // Prepare Assistant message container
        state.isGenerating = true;
        elements.btnSendMsg.classList.add('hidden');
        elements.btnStopGen.classList.remove('hidden');

        const assistantBubble = appendMessage('assistant', '');
        assistantBubble.innerHTML = '<span class="typing-cursor"></span>';

        // Build messages payload
        const payloadMessages = [];
        if (state.systemPrompt.trim()) {
            payloadMessages.push({ role: 'system', content: state.systemPrompt.trim() });
        }
        for (const msg of state.messages) {
            payloadMessages.push({ role: msg.role, content: msg.content });
        }

        state.abortController = new AbortController();

        let accumulatedText = '';

        try {
            const response = await fetch('/api/chat', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({
                    messages: payloadMessages,
                    temperature: state.temperature,
                    stream: true
                }),
                signal: state.abortController.signal
            });

            if (!response.ok) {
                const errData = await response.json().catch(() => ({}));
                throw new Error(errData.message || `HTTP fel ${response.status}`);
            }

            const reader = response.body.getReader();
            const decoder = new TextDecoder('utf-8');
            let buffer = '';

            while (true) {
                const { done, value } = await reader.read();
                if (done) break;

                buffer += decoder.decode(value, { stream: true });
                const lines = buffer.split('\n');
                buffer = lines.pop(); // keep remainder

                for (const line of lines) {
                    const trimmed = line.trim();
                    if (!trimmed || !trimmed.startsWith('data: ')) continue;
                    const jsonStr = trimmed.slice(6);
                    try {
                        const event = JSON.parse(jsonStr);
                        if (event.type === 'token') {
                            accumulatedText += event.piece;
                            assistantBubble.innerHTML = renderMarkdown(accumulatedText) + '<span class="typing-cursor"></span>';
                            scrollToBottom();
                        } else if (event.type === 'done') {
                            if (event.used_ctx && event.n_ctx) {
                                updateContextMeter(event.used_ctx, event.n_ctx);
                            }
                        } else if (event.type === 'error') {
                            throw new Error(event.message || 'Serverfel');
                        }
                    } catch (e) {
                        if (e.message !== 'JSON.parse') {
                            console.error('SSE Error:', e);
                        }
                    }
                }
            }

            // Finish stream rendering
            assistantBubble.innerHTML = renderMarkdown(accumulatedText);
            state.messages.push({ role: 'assistant', content: accumulatedText });

        } catch (err) {
            if (err.name === 'AbortError') {
                assistantBubble.innerHTML = renderMarkdown(accumulatedText + '\n\n*(Generering stoppades av användaren)*');
                state.messages.push({ role: 'assistant', content: accumulatedText });
            } else {
                assistantBubble.innerHTML = `<span style="color: var(--danger)">⚠️ Fel: ${escapeHtml(err.message)}</span>`;
            }
        } finally {
            state.isGenerating = false;
            state.abortController = null;
            elements.btnSendMsg.classList.remove('hidden');
            elements.btnStopGen.classList.add('hidden');
            checkServerStatus();
        }
    }

    // --- Event Listeners ---

    // Chat form submit
    elements.chatForm.addEventListener('submit', (e) => {
        e.preventDefault();
        handleSendMessage(elements.userInput.value);
    });

    // Enter to submit, Shift+Enter for newline
    elements.userInput.addEventListener('keydown', (e) => {
        if (e.key === 'Enter' && !e.shiftKey) {
            e.preventDefault();
            handleSendMessage(elements.userInput.value);
        }
    });

    // Auto-resize input textarea
    elements.userInput.addEventListener('input', () => {
        elements.userInput.style.height = 'auto';
        elements.userInput.style.height = `${Math.min(elements.userInput.scrollHeight, 180)}px`;
    });

    // Stop generation
    elements.btnStopGen.addEventListener('click', () => {
        if (state.abortController) {
            state.abortController.abort();
        }
    });

    // Quick prompt buttons
    document.addEventListener('click', (e) => {
        const btn = e.target.closest('.quick-prompt-btn');
        if (btn) {
            const prompt = btn.getAttribute('data-prompt');
            if (prompt) {
                elements.userInput.value = prompt;
                handleSendMessage(prompt);
            }
        }
    });

    // Temperature slider
    elements.tempSlider.addEventListener('input', (e) => {
        state.temperature = parseFloat(e.target.value);
        elements.tempVal.textContent = state.temperature.toFixed(2);
    });

    // System prompt input
    elements.systemPromptInput.addEventListener('input', (e) => {
        state.systemPrompt = e.target.value;
    });

    // New Chat / Clear Chat
    function clearChatView() {
        state.messages = [];
        elements.messagesContainer.innerHTML = `
            <div class="welcome-screen" id="welcome-screen">
                <div class="welcome-icon">⚡</div>
                <h2>Välkommen till Local AI Web UI</h2>
                <p>Denna webbplats kommunicerar direkt med din lokala AI-server via realtidsstreaming och full context-övervakning.</p>
                <div class="quick-prompts">
                    <button class="quick-prompt-btn" data-prompt="Förklara hur LLM-inference och context fönster fungerar.">
                        💡 Förklara LLM-inference & context
                    </button>
                    <button class="quick-prompt-btn" data-prompt="Skriv en snabb och effektiv C++ funktion för parallell beräkning med trådar.">
                        💻 Skriv en C++ funktion
                    </button>
                    <button class="quick-prompt-btn" data-prompt="Hur bryter man bäst ner en komplex programmeringsuppgift i deluppgifter?">
                        📋 Bryt ner komplexa uppgifter
                    </button>
                </div>
            </div>
        `;
    }

    elements.newChatBtn.addEventListener('click', clearChatView);
    elements.btnClearChat.addEventListener('click', clearChatView);

    // Reset Context (/reset)
    elements.btnResetContext.addEventListener('click', async () => {
        if (!confirm('Vill du återställa serverns context och minne?')) return;
        try {
            const res = await fetch('/api/reset', { method: 'POST' });
            const data = await res.json();
            if (data.type === 'ok' || data.status === 'ok') {
                updateContextMeter(0, state.nCtx);
                alert('Serverns context har återställts!');
            } else {
                alert('Kunde inte återställa context: ' + (data.message || 'Okänt fel'));
            }
        } catch (err) {
            alert('Fel vid anrop till server: ' + err.message);
        }
    });

    // Refresh Status button
    elements.btnRefreshStatus.addEventListener('click', () => {
        checkServerStatus();
    });

    // Mobile menu toggle
    elements.mobileMenuToggle.addEventListener('click', () => {
        elements.sidebar.classList.toggle('open');
    });

    // Server Config Modal
    elements.btnOpenServerModal.addEventListener('click', async () => {
        try {
            const res = await fetch('/api/config');
            const data = await res.json();
            if (data.server_host) elements.cfgServerHost.value = data.server_host;
            if (data.server_port) elements.cfgServerPort.value = data.server_port;
        } catch (e) {}
        elements.serverModal.classList.remove('hidden');
    });

    function closeServerModal() {
        elements.serverModal.classList.add('hidden');
    }

    elements.btnCloseModal.addEventListener('click', closeServerModal);
    elements.btnCancelModal.addEventListener('click', closeServerModal);

    elements.btnSaveModal.addEventListener('click', async () => {
        const host = elements.cfgServerHost.value.trim();
        const port = parseInt(elements.cfgServerPort.value.trim(), 10);
        if (!host || isNaN(port)) {
            alert('Ange giltig värd och port');
            return;
        }

        try {
            const res = await fetch('/api/config', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ server_host: host, server_port: port })
            });
            const data = await res.json();
            closeServerModal();
            checkServerStatus();
        } catch (err) {
            alert('Kunde inte spara konfiguration: ' + err.message);
        }
    });

    // Initial check & interval polling
    checkServerStatus();
    setInterval(checkServerStatus, 5000);
});
