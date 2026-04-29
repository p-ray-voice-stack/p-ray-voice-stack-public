const startButton = document.getElementById("start-demo");
const statusText = document.getElementById("status");
const userTurn = document.getElementById("user-turn");
const assistantTurn = document.getElementById("assistant-turn");
const audioNote = document.getElementById("audio-note");
const answerAudio = document.getElementById("answer");

const demoTurn = {
  user: "Hello P-Ray, show me the smallest local-v2 path.",
  assistant:
    "Local v2 starts with a browser-first demo, then moves to the official hardware baseline after the interaction model is clear.",
  audioNote:
    "Audio output is still a placeholder here. Real recording, STT, TTS, and streaming stay out of Task 3.",
};

function delay(ms) {
  return new Promise((resolve) => {
    window.setTimeout(resolve, ms);
  });
}

async function runDemo() {
  startButton.disabled = true;
  statusText.textContent = "Simulating a local-v2 turn...";
  userTurn.textContent = "Preparing demo input...";
  assistantTurn.textContent = "Waiting for simulated assistant reply...";
  audioNote.textContent = "Resetting the placeholder audio panel...";
  answerAudio.removeAttribute("src");
  answerAudio.load();

  await delay(350);
  userTurn.textContent = demoTurn.user;

  await delay(450);
  assistantTurn.textContent = demoTurn.assistant;
  audioNote.textContent = demoTurn.audioNote;
  statusText.textContent = "Demo complete. No hardware, microphone, or cloud path was required.";
  startButton.disabled = false;
}

startButton.addEventListener("click", () => {
  void runDemo();
});
