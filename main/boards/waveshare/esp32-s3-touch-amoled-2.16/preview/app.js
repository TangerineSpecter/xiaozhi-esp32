const assets = {
    neutral: "../assets/emoji/neutral.png",
    recording: "../assets/emoji/recording.png",
    notes: "../assets/emoji/taking_notes.png",
};

const state = {
    page: "closed",
    selection: 0,
    volume: 70,
    audio: false,
    notes: false,
    recordingStartedAt: 0,
    chatListening: false,
};

const elements = {
    character: document.querySelector("#character"),
    voiceStatus: document.querySelector("#voiceStatus"),
    speechText: document.querySelector("#speechText"),
    recordingBadge: document.querySelector("#recordingBadge"),
    recordingTime: document.querySelector("#recordingTime"),
    notesBadge: document.querySelector("#notesBadge"),
    toast: document.querySelector("#toast"),
    menuOverlay: document.querySelector("#menuOverlay"),
    menuTitle: document.querySelector("#menuTitle"),
    menuGrid: document.querySelector("#menuGrid"),
    menuCards: [...document.querySelectorAll(".menu-card")],
    audioMenuLabel: document.querySelector("#audioMenuLabel"),
    notesMenuLabel: document.querySelector("#notesMenuLabel"),
    volumePage: document.querySelector("#volumePage"),
    volumeValue: document.querySelector("#volumeValue"),
    volumeFill: document.querySelector("#volumeFill"),
    volumeTrack: document.querySelector(".volume-track"),
    menuHint: document.querySelector("#menuHint"),
    centerButton: document.querySelector("#centerButton"),
    statePage: document.querySelector("#statePage"),
    stateAudio: document.querySelector("#stateAudio"),
    stateNotes: document.querySelector("#stateNotes"),
    stateCharacter: document.querySelector("#stateCharacter"),
    stateVolume: document.querySelector("#stateVolume"),
};

let toastTimer = 0;
let holdTimer = 0;
let holdTriggered = false;
let volumePointerActive = false;

function formatElapsed(milliseconds) {
    const totalSeconds = Math.max(0, Math.floor(milliseconds / 1000));
    const minutes = String(Math.floor(totalSeconds / 60)).padStart(2, "0");
    const seconds = String(totalSeconds % 60).padStart(2, "0");
    return `${minutes}:${seconds}`;
}

function currentCharacter() {
    if (state.audio) return [assets.recording, "recording.png"];
    if (state.notes) return [assets.notes, "taking_notes.png"];
    return [assets.neutral, "neutral.png"];
}

function swapCharacter(src) {
    if (elements.character.getAttribute("src") === src) return;
    elements.character.classList.add("switching");
    window.setTimeout(() => {
        elements.character.src = src;
        elements.character.classList.remove("switching");
    }, 110);
}

function showToast(message) {
    window.clearTimeout(toastTimer);
    elements.toast.textContent = message;
    elements.toast.hidden = false;
    toastTimer = window.setTimeout(() => {
        elements.toast.hidden = true;
    }, 1800);
}

function updateStateLabel(element, enabled, onText) {
    element.textContent = enabled ? onText : "未启动";
    element.className = enabled ? (element === elements.stateNotes ? "notes-on" : "on") : "off";
}

function render() {
    const menuOpen = state.page !== "closed";
    const volumeOpen = state.page === "volume";
    const [characterSrc, characterName] = currentCharacter();

    swapCharacter(characterSrc);
    elements.recordingBadge.hidden = !state.audio;
    elements.notesBadge.hidden = !state.notes;
    elements.notesBadge.classList.toggle("with-recording", state.audio && state.notes);
    elements.menuOverlay.hidden = !menuOpen;
    elements.menuGrid.hidden = volumeOpen;
    elements.volumePage.hidden = !volumeOpen;
    elements.menuTitle.textContent = volumeOpen ? "音量设置" : "设置";
    elements.menuHint.innerHTML = volumeOpen
        ? "上 / 下 调整 · 点击或拖动滑条<br>中键保存 · 长按中键 2 秒取消"
        : "上 / 下 选择 · 中键确认<br>点击卡片执行 · 长按中键 2 秒返回";
    elements.audioMenuLabel.textContent = state.audio ? "停止录音" : "录音";
    elements.notesMenuLabel.textContent = state.notes ? "停止记录" : "记录";
    elements.volumeValue.textContent = `${state.volume}%`;
    elements.volumeFill.style.width = `${state.volume}%`;

    elements.menuCards.forEach((card, index) => {
        card.classList.toggle("selected", index === state.selection);
    });

    if (state.audio) {
        elements.voiceStatus.textContent = "录音中";
        elements.speechText.textContent = state.notes
            ? "正在录音，同时记录识别到的文字。"
            : "录音进行中，说“停止录音”或从菜单停止。";
    } else if (state.notes) {
        elements.voiceStatus.textContent = "等待识别";
        elements.speechText.textContent = "文字记录进行中，联网识别结果会持续写入 TF 卡。";
    } else {
        elements.voiceStatus.textContent = state.chatListening ? "聆听中" : "待命";
        elements.speechText.textContent = state.chatListening
            ? "我在听。再次按下键可结束聆听。"
            : "按中键打开设置，试试录音和文字记录。";
    }

    elements.statePage.textContent =
        state.page === "closed" ? "主界面" : state.page === "settings" ? "设置菜单" : "音量设置";
    updateStateLabel(elements.stateAudio, state.audio, "录音中");
    updateStateLabel(elements.stateNotes, state.notes, "记录中");
    elements.stateCharacter.textContent = characterName;
    elements.stateVolume.textContent = `${state.volume}%`;
}

function move(direction) {
    if (state.page === "closed") {
        if (direction < 0) {
            state.chatListening = !state.chatListening;
            showToast(state.chatListening ? "开始聆听" : "结束聆听");
        }
        render();
        return;
    }

    if (state.page === "volume") {
        state.volume = Math.min(100, Math.max(0, state.volume + direction * 5));
    } else {
        state.selection = (state.selection + (direction > 0 ? 1 : 3)) % 4;
    }
    render();
}

function confirm() {
    if (state.page === "closed") {
        state.page = "settings";
        state.selection = 0;
        render();
        return;
    }

    if (state.page === "volume") {
        state.page = "settings";
        showToast(`音量已保存：${state.volume}%`);
        render();
        return;
    }

    switch (state.selection) {
        case 0:
            state.page = "volume";
            break;
        case 1:
            state.audio = !state.audio;
            if (state.audio) {
                state.recordingStartedAt = Date.now();
                showToast("录音已开始");
            } else {
                showToast("录音已保存到 TF 卡");
            }
            break;
        case 2:
            state.notes = !state.notes;
            showToast(state.notes ? "文字记录已开始" : "文字记录已保存");
            break;
        default:
            state.page = "closed";
            break;
    }
    render();
}

function goBack() {
    if (state.page === "volume") {
        state.page = "settings";
        showToast("已取消音量调整");
    } else if (state.page === "settings") {
        state.page = "closed";
    }
    render();
}

function selectMenuCard(index) {
    if (state.page !== "settings") return;
    state.selection = index;
    confirm();
}

function setVolumeFromPointer(event) {
    const bounds = elements.volumeTrack.getBoundingClientRect();
    const ratio = (event.clientX - bounds.left) / bounds.width;
    state.volume = Math.round(Math.min(1, Math.max(0, ratio)) * 20) * 5;
    render();
}

function startCenterHold(event) {
    if (event) event.preventDefault();
    holdTriggered = false;
    elements.centerButton.classList.add("pressed", "holding");
    window.clearTimeout(holdTimer);
    holdTimer = window.setTimeout(() => {
        holdTriggered = true;
        elements.centerButton.classList.remove("holding");
        goBack();
    }, 2000);
}

function endCenterHold(event) {
    if (event) event.preventDefault();
    window.clearTimeout(holdTimer);
    elements.centerButton.classList.remove("pressed", "holding");
    if (!holdTriggered) confirm();
}

function cancelCenterHold() {
    window.clearTimeout(holdTimer);
    elements.centerButton.classList.remove("pressed", "holding");
}

document.querySelector("#upButton").addEventListener("click", () => move(1));
document.querySelector("#downButton").addEventListener("click", () => move(-1));
elements.menuCards.forEach((card) => {
    card.addEventListener("click", () => selectMenuCard(Number(card.dataset.index)));
});
elements.menuOverlay.addEventListener("click", (event) => {
    if (event.target === elements.menuOverlay) goBack();
});
elements.volumeTrack.addEventListener("pointerdown", (event) => {
    if (state.page !== "volume") return;
    volumePointerActive = true;
    elements.volumeTrack.setPointerCapture(event.pointerId);
    setVolumeFromPointer(event);
});
elements.volumeTrack.addEventListener("pointermove", (event) => {
    if (volumePointerActive) setVolumeFromPointer(event);
});
elements.volumeTrack.addEventListener("pointerup", (event) => {
    volumePointerActive = false;
    elements.volumeTrack.releasePointerCapture(event.pointerId);
});
elements.volumeTrack.addEventListener("pointercancel", () => {
    volumePointerActive = false;
});
elements.centerButton.addEventListener("pointerdown", startCenterHold);
elements.centerButton.addEventListener("pointerup", endCenterHold);
elements.centerButton.addEventListener("pointercancel", cancelCenterHold);
elements.centerButton.addEventListener("pointerleave", (event) => {
    if (event.buttons) cancelCenterHold();
});

document.querySelector("#resetButton").addEventListener("click", () => {
    Object.assign(state, {
        page: "closed",
        selection: 0,
        volume: 70,
        audio: false,
        notes: false,
        recordingStartedAt: 0,
        chatListening: false,
    });
    showToast("模拟器已重置");
    render();
});

document.addEventListener("keydown", (event) => {
    if (event.repeat && event.key !== "Enter") return;
    if (event.key === "ArrowUp") {
        event.preventDefault();
        move(1);
    } else if (event.key === "ArrowDown") {
        event.preventDefault();
        move(-1);
    } else if (event.key === "Enter" && !event.repeat) {
        event.preventDefault();
        startCenterHold();
    }
});

document.addEventListener("keyup", (event) => {
    if (event.key === "Enter") {
        event.preventDefault();
        endCenterHold();
    }
});

window.setInterval(() => {
    if (state.audio) {
        elements.recordingTime.textContent = formatElapsed(Date.now() - state.recordingStartedAt);
    }
}, 250);

render();
