-- The few lines the connection screens show, in English and Korean (host.language()).
local lines = {
  en = {
    login = "Login",
    offline = "Play offline",
    connecting = "Logging in...",
    lobby = "Looking for a channel...",
    entering = "Entering Channel %d...",
    failed = "Could not reach the server.",
    full = "Every channel is full.",
    closed = "No channel is open right now.",
    busy = "The server is busy. Try again in a moment.",
    refused = "The server refused the login.",
    lost = "The connection to the server was lost.",
    ok = "OK",
  },
  ko = {
    login = "로그인",
    offline = "오프라인 플레이",
    connecting = "로그인 중...",
    lobby = "채널을 찾는 중...",
    entering = "채널 %d 입장 중...",
    failed = "서버에 접속할 수 없습니다.",
    full = "모든 채널이 가득 찼습니다.",
    closed = "지금 열려 있는 채널이 없습니다.",
    busy = "서버가 혼잡합니다. 잠시 후 다시 시도해 주세요.",
    refused = "서버가 로그인을 거부했습니다.",
    lost = "서버와의 연결이 끊겼습니다.",
    ok = "확인",
  },
}

local Text = {}

function Text.get(key, ...)
  local language = lines[host.language()] or lines.en
  return string.format(language[key] or lines.en[key] or key, ...)
end

return Text
