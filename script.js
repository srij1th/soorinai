const API = "http://localhost:8080";
const REQUEST_TIMEOUT_MS = 6000;

const PIECE_MAP = {
  P: "♙", N: "♘", B: "♗", R: "♖", Q: "♕", K: "♔",
  p: "♟", n: "♞", b: "♝", r: "♜", q: "♛", k: "♚",
  ".": ""
};

const appState = {
  board: null,
  legalMoves: [],
  selected: null,
  lastMove: null,
  moveHistory: [],
  timeLeft: 240,
  timerStarted: false,
  yourTurn: false,
  playerColor: "white",
  playerScore: 0,
  aiScore: 0
};

let timerInterval = null;
let gameEndShown = false;
let redirectInterval = null;

function setMessage(id, text, ok) {
  const el = document.getElementById(id);
  if (!el) return;
  el.textContent = text || "";
  el.style.color = ok ? "#166534" : "#b91c1c";
}

async function fetchWithTimeout(url, options) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), REQUEST_TIMEOUT_MS);
  try {
    return await fetch(url, { ...options, signal: controller.signal });
  } finally {
    clearTimeout(timer);
  }
}

async function apiPost(path, data) {
  const res = await fetchWithTimeout(API + path, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(data || {})
  });
  return res.json();
}

async function apiGet(path) {
  const url = path + (path.includes("?") ? "&" : "?") + "_t=" + Date.now();
  const res = await fetchWithTimeout(API + url, {
    cache: "no-store"
  });
  return res.json();
}

function go(page) {
  window.location.href = page;
}

function currentPage() {
  return document.body.dataset.page || "";
}

function saveUsername(name) {
  localStorage.setItem("chess_user", name);
}

function getUsername() {
  return localStorage.getItem("chess_user") || "";
}

function saveCredentials(username, password) {
  localStorage.setItem("chess_user", username || "");
  localStorage.setItem("chess_pass", password || "");
}

function getSavedCredentials() {
  return {
    username: localStorage.getItem("chess_user") || "",
    password: localStorage.getItem("chess_pass") || ""
  };
}

async function ensureSession() {
  try {
    const stats = await apiGet("/get-stats");
    if (stats && stats.ok) return true;
  } catch (e) {
    /* Try auto-login below. */
  }

  const creds = getSavedCredentials();
  if (!creds.username || !creds.password) return false;

  try {
    const login = await apiPost("/login", {
      username: creds.username,
      password: creds.password
    });
    return !!(login && login.ok);
  } catch (e) {
    return false;
  }
}

function applyTheme(theme) {
  if (theme === "dark") document.body.classList.add("dark-mode");
  else document.body.classList.remove("dark-mode");
  localStorage.setItem("chess_theme", theme);
}

function applyBoardStyle(style) {
  if (String(style) === "1") {
    document.body.classList.add("board-green");
  } else {
    document.body.classList.remove("board-green");
  }
  localStorage.setItem("chess_board_style", String(style));
}

function loadSavedTheme() {
  applyTheme(localStorage.getItem("chess_theme") || "light");
  applyBoardStyle(localStorage.getItem("chess_board_style") || "0");
}

async function initLoginPage() {
  const loginBtn = document.getElementById("loginBtn");
  const signupBtn = document.getElementById("signupBtn");

  loginBtn.addEventListener("click", async () => {
    const username = document.getElementById("username").value.trim();
    const password = document.getElementById("password").value.trim();
    try {
      const data = await apiPost("/login", { username, password });
      if (!data.ok) {
        setMessage("message", data.error || "Login failed.", false);
        return;
      }
      saveCredentials(username, password);
      saveUsername(data.username);
      go("dashboard.html");
    } catch (e) {
      setMessage("message", "Server not reachable. Run chess_server and open via localhost:8080.", false);
    }
  });

  signupBtn.addEventListener("click", async () => {
    const username = document.getElementById("username").value.trim();
    const password = document.getElementById("password").value.trim();
    try {
      const data = await apiPost("/signup", { username, password });
      if (!data.ok) {
        setMessage("message", data.error || "Signup failed.", false);
        return;
      }
      setMessage("message", "Account created. Now login.", true);
    } catch (e) {
      setMessage("message", "Server not reachable. Run chess_server and open via localhost:8080.", false);
    }
  });
}

async function initDashboardPage() {
  const ok = await ensureSession();
  if (!ok) {
    setMessage("message", "Session expired. Please login again.", false);
    setTimeout(() => go("index.html"), 900);
    return;
  }
  const user = getUsername();
  document.getElementById("welcomeText").textContent = `Welcome, ${user}`;

  async function startGameWithColor(playerColor) {
    const btnW = document.getElementById("playWhiteBtn");
    const btnB = document.getElementById("playBlackBtn");
    [btnW, btnB].forEach((b) => {
      if (b) b.disabled = true;
    });
    setMessage("message", "Loading game...", true);
    try {
      const data = await apiPost("/start-game", { playerColor });
      if (!data.ok) {
        setMessage("message", data.error || "Could not start game.", false);
        return;
      }
      go("game.html");
    } catch (e) {
      setMessage("message", "Server timeout. Check server terminal and refresh.", false);
    } finally {
      [btnW, btnB].forEach((b) => {
        if (b) b.disabled = false;
      });
    }
  }

  document.getElementById("playWhiteBtn").addEventListener("click", () => {
    startGameWithColor("white");
  });
  document.getElementById("playBlackBtn").addEventListener("click", () => {
    startGameWithColor("black");
  });
  document.getElementById("statsBtn").addEventListener("click", () => go("stats.html"));
  document.getElementById("settingsBtn").addEventListener("click", () => go("settings.html"));
  document.getElementById("instructionsBtn").addEventListener("click", () => go("instructions.html"));
  document.getElementById("logoutBtn").addEventListener("click", async () => {
    await apiPost("/logout", {});
    localStorage.removeItem("chess_user");
    go("index.html");
  });
}

function validTargetsForSelected() {
  if (!appState.selected || !appState.yourTurn) return [];
  return appState.legalMoves.filter(
    (m) => m.fr === appState.selected.r && m.fc === appState.selected.c
  );
}

function renderBoard() {
  const boardEl = document.getElementById("board");
  if (!boardEl) return;
  boardEl.innerHTML = "";

  const valids = validTargetsForSelected();
  const rows = appState.board && Array.isArray(appState.board) ? appState.board.length : 0;
  if (rows !== 8) {
    for (let r = 0; r < 8; r++) {
      for (let c = 0; c < 8; c++) {
        const sq = document.createElement("div");
        const isLight = (r + c) % 2 === 0;
        sq.className = `square ${isLight ? "light" : "dark"}`;
        sq.dataset.r = String(r);
        sq.dataset.c = String(c);
        boardEl.appendChild(sq);
      }
    }
    return;
  }

  for (let r = 0; r < 8; r++) {
    for (let c = 0; c < 8; c++) {
      const sq = document.createElement("div");
      const isLight = (r + c) % 2 === 0;
      const pieceCode = appState.board[r][c];
      sq.className = `square ${isLight ? "light" : "dark"}`;
      sq.dataset.r = String(r);
      sq.dataset.c = String(c);
      sq.textContent = "";
      if (pieceCode && pieceCode !== ".") {
        const pieceEl = document.createElement("span");
        pieceEl.className = "piece";
        if (pieceCode >= "A" && pieceCode <= "Z") pieceEl.classList.add("piece-white");
        else pieceEl.classList.add("piece-black");
        pieceEl.textContent = PIECE_MAP[pieceCode] || "";
        sq.appendChild(pieceEl);
      }

      if (appState.selected && appState.selected.r === r && appState.selected.c === c) {
        sq.classList.add("selected");
      }
      if (valids.some((m) => m.tr === r && m.tc === c)) {
        sq.classList.add("valid");
      }
      if (
        appState.lastMove &&
        ((appState.lastMove.fr === r && appState.lastMove.fc === c) ||
         (appState.lastMove.tr === r && appState.lastMove.tc === c))
      ) {
        sq.classList.add("last-move");
      }

      sq.addEventListener("click", () => onSquareClick(r, c));
      boardEl.appendChild(sq);
    }
  }
}

function renderMoveLog() {
  const list = document.getElementById("moveLogList");
  if (!list) return;
  list.innerHTML = "";
  appState.moveHistory.forEach((mv) => {
    const li = document.createElement("li");
    li.textContent = mv.replace(" ", " -> ");
    list.appendChild(li);
  });
}

function formatTime(total) {
  const minutes = Math.floor(total / 60);
  const seconds = total % 60;
  return `${String(minutes).padStart(2, "0")}:${String(seconds).padStart(2, "0")}`;
}

function stopTimerTicker() {
  if (timerInterval) {
    clearInterval(timerInterval);
    timerInterval = null;
  }
}

function stopRedirectTicker() {
  if (redirectInterval) {
    clearInterval(redirectInterval);
    redirectInterval = null;
  }
}

function showGameEndPopup(resultText) {
  apiPost("/update-stats", {});
  const modal = document.getElementById("gameEndModal");
  const resultEl = document.getElementById("modalResultText");
  const redirectEl = document.getElementById("modalRedirectText");
  const backBtn = document.getElementById("backMenuModalBtn");
  let seconds = 5;

  if (!modal || !resultEl || !redirectEl || !backBtn) return;
  if (gameEndShown) return;
  gameEndShown = true;

  resultEl.textContent = resultText;
  redirectEl.textContent = `Returning to menu in ${seconds}s...`;
  modal.classList.remove("hidden");

  backBtn.onclick = () => go("dashboard.html");
  stopRedirectTicker();
  redirectInterval = setInterval(() => {
    seconds -= 1;
    redirectEl.textContent = `Returning to menu in ${seconds}s...`;
    if (seconds <= 0) {
      stopRedirectTicker();
      go("dashboard.html");
    }
  }, 1000);
}

function startTimerTicker() {
  stopTimerTicker();
  timerInterval = setInterval(() => {
    if (!appState.timerStarted) return;
    if (appState.timeLeft > 0) {
      appState.timeLeft -= 1;
    }
    const timerEl = document.getElementById("timerText");
    if (timerEl) timerEl.textContent = `Time Left: ${formatTime(appState.timeLeft)}`;

    if (appState.timeLeft <= 0 && !gameEndShown) {
      stopTimerTicker();
      if (appState.playerScore > appState.aiScore) showGameEndPopup("Player Wins");
      else if (appState.aiScore > appState.playerScore) showGameEndPopup("AI Wins");
      else showGameEndPopup("Draw");
    }
  }, 1000);
}

function updateResultMessage(status) {
  if (status === "checkmate_player_wins") return "Player Wins";
  if (status === "checkmate_player_loses") return "AI Wins";
  if (status === "timeout_player_wins") return "Player Wins (Time Up)";
  if (status === "timeout_ai_wins") return "AI Wins (Time Up)";
  if (status === "timeout_draw") return "Draw (Time Up)";
  if (status === "stalemate_draw") return "Draw (Stalemate)";
  return status || "normal";
}

function syncPlayerSideLabel(data) {
  const el = document.getElementById("playerColorText");
  if (!el || !data || data.ok === false) return;
  const c = String(data.playerColor || "white").toLowerCase();
  appState.playerColor = c;
  if (c === "black") {
    el.textContent = "You are Black — AI (White) moves first.";
  } else {
    el.textContent = "You are White — you move first.";
  }
}

function updateGameInfoFromData(data) {
  appState.timerStarted = !!data.timerStarted;
  appState.yourTurn = !!data.yourTurn;
  appState.timeLeft = Number(data.timeLeft || 0);
  appState.playerScore = Number((data.scores && data.scores.player) || 0);
  appState.aiScore = Number((data.scores && data.scores.ai) || 0);

  const timerEl = document.getElementById("timerText");
  const playerScoreEl = document.getElementById("playerScoreBox");
  const aiScoreEl = document.getElementById("aiScoreBox");
  syncPlayerSideLabel(data);

  if (timerEl) {
    if (!appState.timerStarted) {
      timerEl.textContent = "Time: Waiting for first move…";
      stopTimerTicker();
    } else {
      timerEl.textContent = `Time Left: ${formatTime(appState.timeLeft)}`;
      startTimerTicker();
    }
  }
  if (playerScoreEl) playerScoreEl.textContent = `Player: ${appState.playerScore}`;
  if (aiScoreEl) aiScoreEl.textContent = `AI: ${appState.aiScore}`;

  if (data.lastGain && data.lastGain.value > 0) {
    if (data.lastGain.mover === 0) setMessage("statusText", `You gained +${data.lastGain.value} points`, true);
    else if (data.lastGain.mover === 1) setMessage("statusText", `AI gained +${data.lastGain.value} points`, false);
  } else {
    setMessage("statusText", updateResultMessage(data.status), true);
  }

  if (
    data.status === "checkmate_player_wins" ||
    data.status === "checkmate_player_loses" ||
    data.status === "timeout_player_wins" ||
    data.status === "timeout_ai_wins" ||
    data.status === "timeout_draw" ||
    data.status === "stalemate_draw"
  ) {
    stopTimerTicker();
    if (!gameEndShown) {
      if (data.status === "checkmate_player_wins") showGameEndPopup("Player Wins");
      else if (data.status === "checkmate_player_loses") showGameEndPopup("AI Wins");
      else if (data.status === "stalemate_draw") showGameEndPopup("Draw");
      else if (appState.playerScore > appState.aiScore) showGameEndPopup("Player Wins");
      else if (appState.aiScore > appState.playerScore) showGameEndPopup("AI Wins");
      else showGameEndPopup("Draw");
    }
  }
}

async function loadBoard() {
  setMessage("statusText", "Loading board...", true);
  try {
    const data = await apiGet("/get-board");
    if (!data.ok) {
      setMessage("statusText", data.error || "No active game.", false);
      return data;
    }
    appState.board = data.board;
    appState.legalMoves = data.legalMoves || [];
    appState.selected = null;
    appState.lastMove = data.lastMove && data.lastMove.valid ? data.lastMove : null;
    appState.moveHistory = data.moveHistory || [];
    document.getElementById("turnText").textContent = `Turn: ${data.turn}`;
    updateGameInfoFromData(data);
    renderBoard();
    renderMoveLog();
    return data;
  } catch (e) {
    setMessage("statusText", "Server timeout. Please refresh or restart server.", false);
    return { ok: false };
  }
}

async function loadBoardAndRunAiIfNeeded() {
  const data = await loadBoard();
  if (data && data.ok && data.aiPending) {
    await doAiMoveWithDelay();
  }
  return data;
}

async function doAiMoveWithDelay() {
  setMessage("statusText", "AI is thinking...", true);
  await new Promise((resolve) => setTimeout(resolve, 1400));
  try {
    const data = await apiPost("/ai-move", {});
    if (!data.ok) {
      setMessage("statusText", data.error || "AI move failed.", false);
      return;
    }
    appState.board = data.board;
    appState.legalMoves = data.legalMoves || [];
    appState.selected = null;
    appState.lastMove = data.lastMove && data.lastMove.valid ? data.lastMove : null;
    appState.moveHistory = data.moveHistory || [];
    document.getElementById("turnText").textContent = `Turn: ${data.turn}`;
    updateGameInfoFromData(data);
    renderBoard();
    renderMoveLog();
  } catch (e) {
    setMessage("statusText", "Server timeout while AI moved.", false);
  }
}

async function onSquareClick(r, c) {
  if (!appState.board) return;
  if (!appState.yourTurn) return;

  if (!appState.selected) {
    const hasMoves = appState.legalMoves.some((m) => m.fr === r && m.fc === c);
    if (hasMoves) {
      appState.selected = { r, c };
      renderBoard();
    }
    return;
  }

  const move = appState.legalMoves.find(
    (m) => m.fr === appState.selected.r && m.fc === appState.selected.c && m.tr === r && m.tc === c
  );

  if (!move) {
    const hasMoves = appState.legalMoves.some((m) => m.fr === r && m.fc === c);
    appState.selected = hasMoves ? { r, c } : null;
    renderBoard();
    return;
  }

  try {
    const data = await apiPost("/move", {
      fr: move.fr, fc: move.fc, tr: move.tr, tc: move.tc
    });
    if (!data.ok) {
      setMessage("statusText", data.error || "Move failed.", false);
      return;
    }

    appState.board = data.board;
    appState.legalMoves = data.legalMoves || [];
    appState.selected = null;
    appState.lastMove = data.lastMove && data.lastMove.valid ? data.lastMove : null;
    appState.moveHistory = data.moveHistory || [];
    document.getElementById("turnText").textContent = `Turn: ${data.turn}`;
    updateGameInfoFromData(data);
    renderBoard();
    renderMoveLog();
    if (data.aiPending) {
      await doAiMoveWithDelay();
    }
  } catch (e) {
    setMessage("statusText", "Server timeout while moving piece.", false);
  }
}

async function initGamePage() {
  const ok = await ensureSession();
  if (!ok) {
    setMessage("statusText", "Session expired. Please login again.", false);
    setTimeout(() => go("index.html"), 900);
    return;
  }
  document.getElementById("refreshBtn").addEventListener("click", loadBoardAndRunAiIfNeeded);
  document.getElementById("backDashboardBtn").addEventListener("click", () => go("dashboard.html"));

  gameEndShown = false;
  stopRedirectTicker();
  stopTimerTicker();
  const modal = document.getElementById("gameEndModal");
  if (modal) modal.classList.add("hidden");

  const data = await loadBoardAndRunAiIfNeeded();
  if (!data || !data.ok) {
    const hint =
      data && data.error && !String(data.error).includes("No active game")
        ? `${data.error} If you just started a game, try Refresh.`
        : "No active game. Go to the Dashboard and choose Play as White or Play as Black.";
    setMessage("statusText", hint, false);
    appState.board = null;
    renderBoard();
    return;
  }
}

async function loadStatsFromServer() {
  const data = await apiGet("/get-stats");
  if (data.ok) {
    document.getElementById("statsUsername").textContent = `Player: ${data.username}`;
    document.getElementById("gamesPlayed").textContent = data.gamesPlayed;
    document.getElementById("wins").textContent = data.wins;
    document.getElementById("losses").textContent = data.losses;
    document.getElementById("draws").textContent = data.draws;
  }
}

async function initStatsPage() {
  const ok = await ensureSession();
  if (!ok) {
    go("index.html");
    return;
  }
  await loadStatsFromServer();
  document.getElementById("backBtn").addEventListener("click", () => go("dashboard.html"));
  window.addEventListener("pageshow", (ev) => {
    if (document.body.dataset.page === "stats" && ev.persisted) {
      loadStatsFromServer();
    }
  });
}

async function initSettingsPage() {
  const keybindingEl = document.getElementById("keybindingStyle");
  const boardStyleEl = document.getElementById("boardStyle");
  const themeEl = document.getElementById("themeMode");
  const saveBtn = document.getElementById("saveSettingsBtn");
  const resetBtn = document.getElementById("resetStatsBtn");
  const backBtn = document.getElementById("backBtn");

  /* Theme should work even if backend is down or user is not logged in. */
  themeEl.value = localStorage.getItem("chess_theme") || "light";
  boardStyleEl.value = localStorage.getItem("chess_board_style") || "0";

  themeEl.addEventListener("change", (e) => {
    applyTheme(e.target.value);
    setMessage("message", `Theme changed to ${e.target.value}.`, true);
  });
  boardStyleEl.addEventListener("change", (e) => {
    applyBoardStyle(e.target.value);
    setMessage("message", "Board colors updated.", true);
  });

  const ok = await ensureSession();
  if (!ok) {
    setMessage("message", "Please login first. Theme still works locally.", false);
    saveBtn.disabled = true;
    resetBtn.disabled = true;
  }

  saveBtn.addEventListener("click", async () => {
    try {
      const keybinding_style = Number(keybindingEl.value);
      const board_style = Number(boardStyleEl.value);
      const data = await apiPost("/settings", { keybinding_style, board_style });
      if (!data.ok) return setMessage("message", data.error || "Could not save settings.", false);
      applyBoardStyle(String(board_style));
      setMessage("message", "Settings saved.", true);
    } catch (e) {
      setMessage("message", "Could not save settings. Check server/login.", false);
    }
  });

  resetBtn.addEventListener("click", async () => {
    const confirmed = window.confirm("Reset all stats (games, wins, losses, draws)?");
    if (!confirmed) return;
    try {
      const data = await apiPost("/reset-stats", {});
      if (!data.ok) return setMessage("message", data.error || "Could not reset stats.", false);
      setMessage("message", "Stats reset successfully.", true);
    } catch (e) {
      setMessage("message", "Server not reachable for reset.", false);
    }
  });

  backBtn.addEventListener("click", () => go("dashboard.html"));
}

function initInstructionsPage() {
  document.getElementById("backBtn").addEventListener("click", () => go("dashboard.html"));
}

window.addEventListener("DOMContentLoaded", async () => {
  loadSavedTheme();
  const page = currentPage();
  if (page === "login") await initLoginPage();
  else if (page === "dashboard") await initDashboardPage();
  else if (page === "game") await initGamePage();
  else if (page === "stats") await initStatsPage();
  else if (page === "settings") await initSettingsPage();
  else if (page === "instructions") initInstructionsPage();
});
