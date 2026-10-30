// Local developer preview of the existing PetLearning APIs.
// No LLM, no microphone, no cookies, no persistent browser storage.
const $ = (id) => document.getElementById(id);
const state = {device:null,token:null,snapshot:null,pendingFeed:null,busy:false,speak:false};
const BUTTON_IDS = ["pet-feed","pet-wave","story-start","story-next","story-repeat",
                    "story-stop","quiz-start","quiz-1","quiz-2"];

function status(message, error=false) {
  const node=$("status");
  node.textContent=message;
  node.classList.toggle("error",error);
  node.classList.toggle("success",!error);
}
function stopSpeaking() {
  if ("speechSynthesis" in window) window.speechSynthesis.cancel();
}
function say(reviewedText) {
  if (!state.speak || !("speechSynthesis" in window)) return;
  if (typeof reviewedText!=="string" || !reviewedText || reviewedText.length>320) return;
  stopSpeaking();
  const utterance=new SpeechSynthesisUtterance(reviewedText);
  utterance.lang="zh-CN";
  utterance.rate=0.85;
  utterance.pitch=1.08;
  window.speechSynthesis.speak(utterance);
}
function busy(value) {
  state.busy=value;
  for (const id of BUTTON_IDS) $(id).disabled=value;
}
async function api(method, endpoint, payload) {
  if (!state.device || !state.token) throw Error("测试设备尚未连接");
  const controller=new AbortController();
  const timeout=setTimeout(() => controller.abort(),5000);
  try {
    const res=await fetch("/v1/devices/"+encodeURIComponent(state.device)+endpoint,{
      method,
      credentials:"omit",
      cache:"no-store",
      headers:{
        Authorization:"Bearer "+state.token,
        ...(payload?{"Content-Type":"application/json"}:{})
      },
      body:payload?JSON.stringify(payload):undefined,
      signal:controller.signal,
      redirect:"error"
    });
    if (!res.ok) {
      if (res.status===401) throw Error("设备验证失败，请核对 ID 和令牌");
      throw Error("服务请求失败（HTTP "+res.status+"）");
    }
    return await res.json();
  } catch(err) {
    if (err?.name==="AbortError") throw Error("服务响应超时，请检查网络");
    throw err;
  } finally {
    clearTimeout(timeout);
  }
}
function selectTab(name) {
  stopSpeaking();
  for (const panel of document.querySelectorAll("[data-panel]"))
    panel.hidden=panel.dataset.panel!==name;
  for (const tab of document.querySelectorAll("[data-tab]")) {
    const chosen=tab.dataset.tab===name;
    tab.classList.toggle("active",chosen);
    tab.setAttribute("aria-pressed",String(chosen));
  }
}
function showPet(snapshot) {
  state.snapshot=snapshot;
  $("pet-stats").textContent="互动记录："+snapshot.interaction_count+" 次 · "+
    (snapshot.quiet?"安静时间":"可以一起玩");
}
function showStory(result) {
  if (typeof result.tts_text==="string" && result.tts_text.length<=320) {
    $("story-text").textContent=result.tts_text;
    say(result.tts_text);
  }
  $("story-status").textContent=result.story_state==="reading"?
    "第 "+result.page+" 页":result.story_state==="finished"?
    "这本故事已读完":"暂时没有可以读的页面";
}
function showQuiz(result) {
  if (typeof result.tts_text==="string" && result.tts_text.length<=320) {
    $("quiz-text").textContent=result.tts_text;
    say(result.tts_text);
  }
  const labels={question:"猜猜谁是正确答案",retry:"再试一次也可以",
                finished:"完成这道题",idle:"准备开始"};
  $("quiz-progress").textContent=labels[result.learning_state]||"一起学习";
}
async function refresh() {
  const [snapshot,story,quiz]=await Promise.all([
    api("GET","/snapshot"),
    api("GET","/story/status"),
    api("GET","/quiz/status")
  ]);
  showPet(snapshot);
  $("story-status").textContent=story.active?"已在第 "+story.page+" 页":"还没有打开绘本";
  $("quiz-progress").textContent=quiz.active?
    "有一道题正在进行":"已经完成 "+quiz.completed+" 道题";
}
async function act(task) {
  if (state.busy) return;
  busy(true);
  try {
    await task();
    status("操作完成");
  } catch(err) {
    status(err?.message||"请求失败，请稍后重试",true);
    stopSpeaking();
  } finally {
    busy(false);
  }
}
function doStory(action) {
  act(async () => {showStory(await api("POST","/story/"+action));});
}
function doQuiz(action,choice) {
  act(async () => {
    const result=await api("POST","/quiz/"+action,choice?{choice}:undefined);
    showQuiz(result);
    if (result.learning_state==="finished") {
      const p=await api("GET","/quiz/status");
      $("quiz-progress").textContent="已完成 "+p.completed+" 道题 · 想玩可以再来一题";
    }
  });
}
async function feed() {
  await act(async () => {
    // Retain the exact event across uncertain network errors until an ACK.
    // Browser page reload is NOT a crash-durable ESP32 outbox.
    if (!state.pendingFeed) {
      if (!globalThis.crypto?.randomUUID) throw Error("当前浏览器不支持安全事件 ID");
      const snap=await api("GET","/snapshot");
      state.pendingFeed={
        schema_version:"1.0",
        message_id:"play_"+crypto.randomUUID().replaceAll("-",""),
        device_id:state.device,
        seq:snap.seq+1,
        type:"pet.event",
        payload:{event:"PET_FEED"}
      };
    }
    const ack=(await api("POST","/events",{events:[state.pendingFeed]})).acks?.[0];
    if (!ack || !ack.accepted)
      throw Error("服务器未确认互动（"+(ack?.reason_code||"无有效 ACK")+"），保留事件供重试");
    if (ack.message_id!==state.pendingFeed.message_id || ack.seq!==state.pendingFeed.seq)
      throw Error("ACK 身份不匹配，保留事件供重试");
    state.pendingFeed=null;
    showPet(await api("GET","/snapshot"));
    const figure=$("pet-figure");
    figure.classList.remove("cheer");
    void figure.offsetWidth;
    figure.classList.add("cheer");
    say("谢谢你！我们一起快乐地玩耍吧。");
  });
}
function disconnect() {
  stopSpeaking();
  state.device=null;
  state.token=null;
  state.snapshot=null;
  state.pendingFeed=null;
  state.speak=false;
  $("device-token").value="";
  $("speak-toggle").setAttribute("aria-pressed","false");
  $("speak-toggle").textContent="🔈 开启朗读";
  $("connect-panel").hidden=false;
  $("experience").hidden=true;
  status("未连接");
}
$("connect-form").addEventListener("submit",(event)=>{
  event.preventDefault();
  if (state.busy) return;
  const device=$("device-id").value.trim();
  const token=$("device-token").value;
  if (!/^[A-Za-z0-9][A-Za-z0-9_-]{2,63}$/.test(device) || token.length<32) {
    status("请输入有效测试设备 ID 和令牌",true);
    return;
  }
  state.device=device;
  state.token=token;
  busy(true);
  refresh().then(()=>{
    $("device-label").textContent="测试设备："+state.device;
    $("device-token").value="";
    $("connect-panel").hidden=true;
    $("experience").hidden=false;
    selectTab("pet");
    status("测试设备已连接");
  }).catch(err=>{
    status(err?.message||"连接失败",true);
    state.device=null;
    state.token=null;
  }).finally(()=>busy(false));
});
$("disconnect").addEventListener("click",disconnect);
for (const tab of document.querySelectorAll("[data-tab]"))
  tab.addEventListener("click",()=>selectTab(tab.dataset.tab));
$("pet-feed").addEventListener("click",feed);
$("pet-wave").addEventListener("click",()=>{
  const figure=$("pet-figure");
  figure.classList.remove("cheer");
  void figure.offsetWidth;
  figure.classList.add("cheer");
  status("小宠物向你挥手（仅本地表情，不修改成长记录）");
  say("你好呀！很高兴见到你。");
});
$("story-start").addEventListener("click",()=>doStory("start"));
$("story-next").addEventListener("click",()=>doStory("next"));
$("story-repeat").addEventListener("click",()=>doStory("repeat"));
$("story-stop").addEventListener("click",()=>doStory("stop"));
$("quiz-start").addEventListener("click",()=>doQuiz("start"));
$("quiz-1").addEventListener("click",()=>doQuiz("answer",1));
$("quiz-2").addEventListener("click",()=>doQuiz("answer",2));
$("speak-toggle").addEventListener("click",()=>{
  state.speak=!state.speak;
  $("speak-toggle").setAttribute("aria-pressed",String(state.speak));
  $("speak-toggle").textContent=state.speak?"🔊 关闭朗读":"🔈 开启朗读";
  if (!state.speak) stopSpeaking();
  if (state.speak && !("speechSynthesis" in window)) {
    state.speak=false;
    $("speak-toggle").setAttribute("aria-pressed","false");
    status("当前浏览器不支持朗读，请使用视觉模式",true);
  }
});
$("speak-stop").addEventListener("click",stopSpeaking);
window.addEventListener("pagehide",stopSpeaking);
