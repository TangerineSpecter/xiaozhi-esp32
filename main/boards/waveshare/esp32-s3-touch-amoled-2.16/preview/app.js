const assets = {
    neutral: "../assets/emoji/neutral.png",
    petting: "./petting-preview.png",
    recording: "../assets/emoji/recording.png",
    notes: "../assets/emoji/taking_notes.png",
};

const state = {
    page: "closed",
    selection: 0,
    fileSelection: 0,
    audioFileSelection: 0,
    noteFileSelection: 0,
    volume: 70,
    audio: false,
    notes: false,
    petting: false,
    recordingStartedAt: 0,
    chatListening: false,
    playingAudio: false,
};

const audioFiles = [
    { name: "142530.wav", date: "今天 14:25", duration: "00:38", size: "1.2 MB" },
    { name: "101208.wav", date: "今天 10:12", duration: "02:14", size: "4.1 MB" },
    { name: "20260920-1834.wav", date: "昨天 18:34", duration: "00:56", size: "1.8 MB" },
];

const noteFiles = [
    {
        name: "142610.txt",
        date: "今天 14:26",
        preview: "[14:26:11] 明天下午提醒我检查会议材料。",
        count: "4 条记录",
    },
    {
        name: "101330.txt",
        date: "今天 10:13",
        preview: "[10:13:08] 记得给家里的植物浇水。",
        count: "7 条记录",
    },
    {
        name: "20260920-1840.txt",
        date: "昨天 18:40",
        preview: "[18:40:22] 周末整理一下录音文件。",
        count: "2 条记录",
    },
];

const elements = {
    character: document.querySelector("#character"),
    headHitArea: document.querySelector("#headHitArea"),
    voiceStatus: document.querySelector("#voiceStatus"),
    speechText: document.querySelector("#speechText"),
    recordingBadge: document.querySelector("#recordingBadge"),
    recordingTime: document.querySelector("#recordingTime"),
    notesBadge: document.querySelector("#notesBadge"),
    toast: document.querySelector("#toast"),
    menuOverlay: document.querySelector("#menuOverlay"),
    menuBack: document.querySelector("#menuBack"),
    menuClose: document.querySelector("#menuClose"),
    menuTitle: document.querySelector("#menuTitle"),
    menuGrid: document.querySelector("#menuGrid"),
    menuCards: [...document.querySelectorAll(".menu-card")],
    filePage: document.querySelector("#filesPage"),
    fileCards: [...document.querySelectorAll(".file-card")],
    audioFilesPage: document.querySelector("#audioFilesPage"),
    notesFilesPage: document.querySelector("#notesFilesPage"),
    audioList: document.querySelector("#audioList"),
    notesList: document.querySelector("#notesList"),
    audioPlayerStatus: document.querySelector("#audioPlayerStatus"),
    notePreviewText: document.querySelector("#notePreviewText"),
    audioMenuLabel: document.querySelector("#audioMenuLabel"),
    notesMenuLabel: document.querySelector("#notesMenuLabel"),
    volumePage: document.querySelector("#volumePage"),
    volumeValue: document.querySelector("#volumeValue"),
    volumeFill: document.querySelector("#volumeFill"),
    volumeTrack: document.querySelector(".volume-track"),
    menuHint: document.querySelector("#menuHint"),
    upButton: document.querySelector("#upButton"),
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
let upHoldTimer = 0;
let upHoldTriggered = false;
let volumePointerActive = false;
let pettingTimer = 0;

function formatElapsed(milliseconds) {
    const totalSeconds = Math.max(0, Math.floor(milliseconds / 1000));
    const minutes = String(Math.floor(totalSeconds / 60)).padStart(2, "0");
    const seconds = String(totalSeconds % 60).padStart(2, "0");
    return `${minutes}:${seconds}`;
}

function currentCharacter() {
    if (state.audio) return [assets.recording, "recording.png"];
    if (state.notes) return [assets.notes, "taking_notes.png"];
    if (state.petting) return [assets.petting, "petting.png"];
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

function renderFileLists() {
    elements.audioList.innerHTML = audioFiles
        .map(
            (file, index) => `
                <button class="file-row ${index === state.audioFileSelection ? "selected" : ""}" data-audio-index="${index}" type="button">
                    <span class="file-row-icon">${state.playingAudio && index === state.audioFileSelection ? "◼" : "▶"}</span>
                    <span class="file-row-copy"><strong>${file.name}</strong><small>${file.date} · ${file.size}</small></span>
                    <time>${file.duration}</time>
                </button>
            `
        )
        .join("");
    elements.notesList.innerHTML = noteFiles
        .map(
            (file, index) => `
                <button class="file-row ${index === state.noteFileSelection ? "selected" : ""}" data-note-index="${index}" type="button">
                    <span class="file-row-icon">▤</span>
                    <span class="file-row-copy"><strong>${file.name}</strong><small>${file.date} · ${file.count}</small></span>
                    <span class="row-chevron">›</span>
                </button>
            `
        )
        .join("");

    const audioFile = audioFiles[state.audioFileSelection];
    elements.audioPlayerStatus.classList.toggle("playing", state.playingAudio);
    elements.audioPlayerStatus.innerHTML = state.playingAudio
        ? `<i></i><span>播放中 · ${audioFile.name} · ${audioFile.duration}</span>`
        : `<i></i><span>中键播放选中录音</span>`;

    const noteFile = noteFiles[state.noteFileSelection];
    elements.notePreviewText.textContent = noteFile.preview;
}

function render() {
    const captureActive = state.audio || state.notes;
    const menuOpen = state.page !== "closed" && !captureActive;
    const volumeOpen = state.page === "volume";
    const filesOpen = state.page === "files";
    const audioFilesOpen = state.page === "audio-files";
    const notesFilesOpen = state.page === "notes-files";
    const [characterSrc, characterName] = currentCharacter();

    elements.character.classList.toggle("petting-preview", state.petting);
    swapCharacter(characterSrc);
    elements.recordingBadge.hidden = !state.audio;
    elements.notesBadge.hidden = !state.notes;
    elements.notesBadge.classList.toggle("with-recording", state.audio && state.notes);
    elements.headHitArea.disabled = state.page !== "closed" || state.audio || state.notes;
    elements.headHitArea.setAttribute("aria-disabled", String(elements.headHitArea.disabled));
    elements.menuOverlay.hidden = !menuOpen;
    elements.menuBack.hidden = !menuOpen;
    elements.menuClose.hidden = !menuOpen;
    elements.menuGrid.hidden = state.page !== "settings";
    elements.volumePage.hidden = !volumeOpen;
    elements.filePage.hidden = !filesOpen;
    elements.audioFilesPage.hidden = !audioFilesOpen;
    elements.notesFilesPage.hidden = !notesFilesOpen;
    elements.menuTitle.textContent = volumeOpen
        ? "音量设置"
        : filesOpen
          ? "文件"
          : audioFilesOpen
            ? "播放录音"
            : notesFilesOpen
              ? "查看记录"
              : "设置";
    elements.menuHint.innerHTML = volumeOpen
        ? "上 / 下 调整 · 点击或拖动滑条<br>点击返回取消 · 中键保存"
        : audioFilesOpen
          ? "上 / 下 选择 · 中键播放/停止<br>点击返回回到文件"
          : notesFilesOpen
            ? "上 / 下选择记录 · 点击查看内容<br>点击返回回到文件"
            : filesOpen
              ? "上 / 下选择 · 中键进入<br>点击返回回到设置"
              : "上 / 下选择 · 中键确认<br>点击返回关闭菜单";
    elements.audioMenuLabel.textContent = state.audio ? "停止录音" : "录音";
    elements.notesMenuLabel.textContent = state.notes ? "停止记录" : "记录";
    elements.volumeValue.textContent = `${state.volume}%`;
    elements.volumeFill.style.width = `${state.volume}%`;

    elements.menuCards.forEach((card, index) => {
        card.classList.toggle("selected", index === state.selection);
    });
    elements.fileCards.forEach((card, index) => {
        card.classList.toggle("selected", index === state.fileSelection);
    });
    renderFileLists();

    if (state.audio) {
        elements.voiceStatus.textContent = "录音中";
        elements.speechText.textContent = "录音进行中，点击对话按钮或菜单按钮结束。";
    } else if (state.notes) {
        elements.voiceStatus.textContent = "等待识别";
        elements.speechText.textContent = "文字记录进行中，点击对话按钮或菜单按钮结束。";
    } else {
        elements.voiceStatus.textContent = state.chatListening ? "聆听中" : "待命";
        elements.speechText.textContent = state.chatListening
            ? "我在听。再次按下键可结束聆听。"
            : "按中键打开设置，试试录音和文字记录。";
    }

    elements.statePage.textContent = captureActive
        ? "主界面"
        : state.page === "closed"
          ? "主界面"
          : state.page === "settings"
            ? "设置菜单"
            : state.page === "volume"
              ? "音量设置"
              : state.page === "files"
                ? "文件菜单"
                : state.page === "audio-files"
                  ? "录音回放"
                  : "记录查看";
    updateStateLabel(elements.stateAudio, state.audio, "录音中");
    updateStateLabel(elements.stateNotes, state.notes, "记录中");
    elements.stateCharacter.textContent = characterName;
    elements.stateVolume.textContent = `${state.volume}%`;
}

function stopCapture() {
    const wasAudio = state.audio;
    const wasNotes = state.notes;
    state.audio = false;
    state.notes = false;
    state.recordingStartedAt = 0;
    state.page = "closed";
    if (wasAudio) {
        showToast("录音已保存到 TF 卡");
    } else if (wasNotes) {
        showToast("文字记录已保存到 TF 卡");
    }
    state.chatListening = false;
    render();
}

function triggerPetting() {
    if (state.page !== "closed" || state.audio || state.notes) return;

    window.clearTimeout(pettingTimer);
    state.petting = true;
    elements.character.src = assets.petting;
    render();

    pettingTimer = window.setTimeout(() => {
        elements.character.src = assets.neutral;
        state.petting = false;
        render();
    }, 980);
}

function move(direction) {
    if (state.page === "closed") {
        if (direction < 0) {
            if (state.audio || state.notes) {
                stopCapture();
                return;
            }
            state.chatListening = !state.chatListening;
            showToast(state.chatListening ? "开始聆听" : "结束聆听");
        }
        render();
        return;
    }

    if (state.audio || state.notes) return;

    if (state.page === "volume") {
        state.volume = Math.min(100, Math.max(0, state.volume + direction * 5));
    } else if (state.page === "files") {
        state.fileSelection = (state.fileSelection + (direction > 0 ? 1 : 1)) % 2;
    } else if (state.page === "audio-files") {
        state.audioFileSelection =
            (state.audioFileSelection + (direction > 0 ? 1 : audioFiles.length - 1)) % audioFiles.length;
        state.playingAudio = false;
    } else if (state.page === "notes-files") {
        state.noteFileSelection =
            (state.noteFileSelection + (direction > 0 ? 1 : noteFiles.length - 1)) % noteFiles.length;
    } else {
        state.selection = (state.selection + (direction > 0 ? 1 : 3)) % 4;
    }
    render();
}

function confirm() {
    if (state.audio || state.notes) {
        stopCapture();
        return;
    }

    if (state.page === "closed") {
        state.petting = false;
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

    if (state.page === "files") {
        state.page = state.fileSelection === 0 ? "audio-files" : "notes-files";
        state.playingAudio = false;
        render();
        return;
    }

    if (state.page === "audio-files") {
        state.playingAudio = !state.playingAudio;
        showToast(state.playingAudio ? "开始播放录音" : "已停止播放");
        render();
        return;
    }

    if (state.page === "notes-files") {
        showToast(`已打开 ${noteFiles[state.noteFileSelection].name}`);
        render();
        return;
    }

    switch (state.selection) {
        case 0:
            state.page = "volume";
            break;
        case 1:
            state.petting = false;
            state.audio = true;
            state.notes = false;
            state.page = "closed";
            state.recordingStartedAt = Date.now();
            showToast("录音已开始");
            break;
        case 2:
            state.petting = false;
            state.audio = false;
            state.notes = true;
            state.page = "closed";
            showToast("文字记录已开始");
            break;
        case 3:
            state.page = "files";
            break;
    }
    render();
}

function goBack() {
    if (state.audio || state.notes) {
        stopCapture();
        return;
    }

    if (state.page === "volume") {
        state.page = "settings";
        showToast("已取消音量调整");
    } else if (state.page === "audio-files" || state.page === "notes-files") {
        state.playingAudio = false;
        state.page = "files";
    } else if (state.page === "files") {
        state.page = "settings";
    } else if (state.page === "settings") {
        state.petting = false;
        state.page = "closed";
    }
    render();
}

function closeMenu() {
    if (state.audio || state.notes) return;
    state.playingAudio = false;
    state.petting = false;
    state.page = "closed";
    showToast("菜单已关闭");
    render();
}

function toggleAudioRecordingShortcut() {
    if (state.page !== "closed") return;
    if (state.notes) {
        showToast("正在文字记录，请先结束记录");
        return;
    }
    if (state.audio) {
        stopCapture();
        return;
    }

    state.petting = false;
    state.audio = true;
    state.recordingStartedAt = Date.now();
    showToast("录音已开始");
    render();
}

function selectMenuCard(index) {
    if (state.page !== "settings") return;
    state.selection = index;
    confirm();
}

function selectFilePage(page) {
    if (state.page !== "files") return;
    state.fileSelection = page === "audio-files" ? 0 : 1;
    state.page = page;
    state.playingAudio = false;
    render();
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
        if (state.audio || state.notes) {
            stopCapture();
        } else {
            goBack();
        }
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

function startUpHold(event) {
    if (event) event.preventDefault();
    upHoldTriggered = false;
    elements.upButton.classList.add("pressed", "holding");
    window.clearTimeout(upHoldTimer);
    upHoldTimer = window.setTimeout(() => {
        upHoldTriggered = true;
        elements.upButton.classList.remove("holding");
        toggleAudioRecordingShortcut();
    }, 1000);
}

function endUpHold(event) {
    if (event) event.preventDefault();
    window.clearTimeout(upHoldTimer);
    elements.upButton.classList.remove("pressed", "holding");
    if (!upHoldTriggered) move(1);
}

function cancelUpHold() {
    window.clearTimeout(upHoldTimer);
    elements.upButton.classList.remove("pressed", "holding");
}

document.querySelector("#downButton").addEventListener("click", () => move(-1));
elements.menuCards.forEach((card) => {
    card.addEventListener("click", () => selectMenuCard(Number(card.dataset.index)));
});
elements.fileCards.forEach((card) => {
    card.addEventListener("click", () => selectFilePage(card.dataset.filePage));
});
elements.audioList.addEventListener("click", (event) => {
    const row = event.target.closest("[data-audio-index]");
    if (!row) return;
    state.audioFileSelection = Number(row.dataset.audioIndex);
    state.playingAudio = false;
    render();
});
elements.notesList.addEventListener("click", (event) => {
    const row = event.target.closest("[data-note-index]");
    if (!row) return;
    state.noteFileSelection = Number(row.dataset.noteIndex);
    render();
});
elements.menuOverlay.addEventListener("click", (event) => {
    if (event.target === elements.menuOverlay) goBack();
});
elements.menuBack.addEventListener("click", goBack);
elements.menuClose.addEventListener("click", closeMenu);
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
elements.upButton.addEventListener("pointerdown", startUpHold);
elements.upButton.addEventListener("pointerup", endUpHold);
elements.upButton.addEventListener("pointercancel", cancelUpHold);
elements.upButton.addEventListener("pointerleave", (event) => {
    if (event.buttons) cancelUpHold();
});
elements.headHitArea.addEventListener("click", triggerPetting);

document.querySelector("#resetButton").addEventListener("click", () => {
    Object.assign(state, {
        page: "closed",
        selection: 0,
        fileSelection: 0,
        audioFileSelection: 0,
        noteFileSelection: 0,
        volume: 70,
        audio: false,
        notes: false,
        petting: false,
        recordingStartedAt: 0,
        chatListening: false,
        playingAudio: false,
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
