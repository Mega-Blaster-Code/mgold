# Referencia: times, modos, voz e vote kick

## Times

### Criar time

```lua
server:createTeam(name, options?) --> Team
```

- Somente servidor.
- `name`: string nao vazia ate 32 bytes, sem caracteres de controle.
- `options.maxPlayers`: limite positivo de membros; padrao e `server.maxPlayers`.
  O valor e convertido para inteiro.
- Nome duplicado ou limite invalido gera erro Lua.
- Ao criar, o servidor envia um snapshot atualizado e emite `TeamCreated`.

### Atribuir player

```lua
server:setPlayerTeam(playerId, teamName?) --> boolean
player:setTeam(teamName?) --> boolean
```

`teamName == nil` remove do time. Retorna `false` se player/time nao existir
ou se o time estiver cheio; caso contrario atualiza o player, replica dados e
emite `PlayerTeamChanged(session, player, teamName)`.

### Consultar e pontuar

```lua
server:getTeams() --> mapa { [nome] = { name, score, maxPlayers } }
team:getPlayers() --> array de Player
team:setScore(score) --> Team
```

- `getTeams()` retorna tabela nova de snapshots; no cliente os times tambem
  sao snapshots simples, nao objetos `Team` com metodos.
- `getPlayers()` retorna array novo com os players ainda conectados.
- `setScore` e somente servidor e exige numero finito. Replica o snapshot,
  emite `TeamScoreChanged(session, team, score)` e retorna o mesmo Team.
- Criacao/atualizacao no cliente emite `TeamsChanged(session, teams)`.

## Modos de jogo

### `love.mgold.gameMode.create(name, callbacks?)`

Retorna uma tabela Lua `{ name = name, ...callbacks }`, registra-a em
`love.mgold.multiplayer.gameModes[name]` e permite usa-la em
`server:setGameMode(modeOrName)`.

- `name`: string nao vazia ate 64 bytes.
- `callbacks`: tabela opcional de funcoes; a tabela e copiada.
- Nomes repetidos substituem o registro anterior.
- Modos predefinidos: `"lobby"`, `"deathmatch"` e
  `"team-deathmatch"` (tambem registrado como `gameModes.teamDeathmatch`).

### Selecionar e controlar partida

```lua
server:setGameMode(modeOrName) --> server
server:startGame() --> boolean
server:returnToLobby() --> true
server:spawnPlayer(playerId) --> boolean
server:playerDied(playerId, killerId?) --> boolean
```

- `setGameMode` aceita nome registrado ou tabela valida com `name`; somente
  servidor e nao pode ser chamado no estado `"playing"`. Chama
  `onInitialize(mode, session)`, emite `GameModeChanged` e replica modo/estado.
- `startGame` retorna `false` se ja estiver em partida; caso contrario marca
  `"playing"`, chama `onStart(mode, session)`, emite `GameStart` e replica.
- `returnToLobby` chama `onEnd(mode, session)`, muda estado para `"lobby"`,
  emite `ReturnToLobby` e replica. Retorna `true`.
- `spawnPlayer` valida ID, emite `PlayerSpawn`; nao instancia personagem.
- `playerDied` valida o jogador morto, resolve `killerId` se existir e emite
  `PlayerDeath`; nao calcula dano/score automaticamente fora do callback do
  modo.

Callbacks implementados e argumentos:

| Callback | Argumentos | Momento |
| --- | --- | --- |
| `onInitialize(mode, session)` | modo, sessao | Modo selecionado. |
| `onStart(mode, session)` | modo, sessao | Partida iniciada. |
| `onEnd(mode, session)` | modo, sessao | Antes de voltar ao lobby. |
| `onPlayerJoin(mode, player)` | modo, Player | Player entrou. |
| `onPlayerLeave(mode, player, reason)` | modo, Player, texto | Player saiu. |
| `onPlayerState(mode, player, state)` | modo, Player, estado | Estado aceito/recebido. |
| `onPlayerPositionCorrected(mode, player, state)` | modo, Player, estado | Estado corrigido. |
| `onPlayerTeamChanged(mode, player, team)` | modo, Player, nome/nil | Time alterado. |
| `onPlayerSpawn(mode, player)` | modo, Player | `spawnPlayer` chamado. |
| `onPlayerDeath(mode, player, killer)` | modo, morto, killer/nil | `playerDied` chamado. |
| `onReturnToLobby(mode, extra)` | modo, modo (mesma referencia) | Retorno ao lobby. |
| `update(mode, session, dt)` | modo, sessao, delta time | Cada atualizacao do servidor. |

Exemplo:

```lua
local mode = love.mgold.gameMode.create("arena", {
    onInitialize = function(self, session)
        self.round = 0
    end,
    onPlayerDeath = function(self, deadPlayer, killer)
        if killer then
            killer:setData("score", (tonumber(killer:getData("score")) or 0) + 1,
                { public = true })
        end
    end,
    update = function(self, session, dt)
        -- Verifique regras de vitoria e chame session:returnToLobby()
        -- ou atualize estado do jogo.
    end,
})

server:setGameMode(mode)
server:startGame()
```

O framework nao cria personagens, nao faz respawn automatico e nao determina
vencedores. O jogo deve chamar `spawnPlayer`, `playerDied` e as operacoes do
seu proprio gameplay.

## Transporte de voz

Voz e transporte de frames, nao implementacao de voice chat completa. Nao ha
captura de microfone, permissao do SO, codec ou playback nativo.

### Configuracao

```lua
voice = {
    enabled = false,
    global = true,
    team = false,
    channels = { proximity = true },
    maxPacketBytes = 4096,
    maxPacketsPerSecond = 50,
    maxDistance = 0,
    canHear = function(sender, listener, channel, session, distance)
        return true
    end,
}
```

- `enabled`: opt-in no servidor; cliente atualiza esta opcao no handshake.
- `global`/`team`: permitem os canais embutidos.
- `channels`: mapa `[nome] = true` dos canais adicionais aceitos.
- `maxPacketBytes`: limite positivo por frame, no maximo `maxPacketBytes`
  global da sessao.
- `maxPacketsPerSecond`: limite por peer e janela de um segundo.
- `maxDistance`: zero desativa limite espacial; valor positivo compara
  distancia entre estados de posicao dos players.
- `canHear`: callback opcional do servidor; precisa retornar exatamente
  `true`. Recebe remetente Player, ouvinte Player, canal, sessao e distancia.
  Se ausente, canal team exige mesmo time e global usa `global`.

### Funcoes do cliente

```lua
client:sendVoice(frame, channel?) --> boolean
client:setVoiceCodec(encode?, decode?) --> client
client:setPushToTalk(active) --> client
client:mutePlayer(id) --> boolean
client:unmutePlayer(id) --> boolean
client:muteAll() --> client
client:unmuteAll() --> client
client:setPlayerVoiceVolume(id, volume) --> client
client:getPlayerVoiceVolume(id) --> number
```

- `sendVoice`: exige cliente conectado e voz habilitada. `frame` deve ser
  string nao vazia dentro do limite depois do encoder. Canal padrao `"global"`;
  PTT habilitado exige `setPushToTalk(true)` antes do envio. `false` indica
  recusa local; `true` indica pacote nao confiavel enfileirado.
- `setVoiceCodec`: encoder recebe frame original e deve retornar bytes string;
  decoder recebe os bytes recebidos e retorna o frame para playback. Ambos
  sao funcoes locais, opcionais.
- `setPushToTalk`: `active` deve ser boolean.
- mute/unmute player: ID invalido retorna `false`; sucesso retorna `true`.
- `muteAll`/`unmuteAll`: liga/desliga mute local geral.
- volume: faixa limitada a 0..1; getter retorna `1` se nao foi configurado.
- Evento `VoicePacket(session, senderId, decodedFrame, channel, volume)` e
  callback `client.voice.onPacket(senderId, decodedFrame, channel, volume)`
  recebem o volume local aplicado logicamente; o jogo ainda controla a
  amplitude do audio.

Exemplo de integracao:

```lua
local client = love.mgold.client.create({
    voice = { enabled = true, pushToTalk = true },
}):connect("127.0.0.1", 27015)

client:setVoiceCodec(encodeAudioFrame, decodeAudioFrame)
client.voice.onPacket = function(senderId, decodedFrame, channel, volume)
    audio:playVoice(senderId, decodedFrame, volume)
end

function love.keypressed(key)
    if key == "v" then client:setPushToTalk(true) end
end

function love.keyreleased(key)
    if key == "v" then client:setPushToTalk(false) end
end
```

## Vote kick

### Opcoes

```lua
voteKick = {
    enabled = false,
    duration = 30,
    requiredVotes = 0.6,
}
```

- `enabled`: desativado por padrao.
- `duration`: segundos positivos ate encerrar automaticamente.
- `requiredVotes`: fracao maior que 0 e menor/igual a 1 dos votantes elegiveis;
  limiar arredondado para cima e minimo de um voto.
- O alvo nao pode votar. Apenas uma votacao pode existir ao mesmo tempo.

### Iniciar e votar

```lua
server:startVoteKick(targetId, callerId?) --> success, reason?
client:requestVoteKick(targetId) --> boolean
client:castVote(yes) --> boolean
```

- `startVoteKick` e API local do servidor. Pode retornar `false` com motivo:
  desativado, votacao em andamento, alvo inexistente ou sem votantes.
  `callerId`, se elegivel, conta como voto positivo inicial.
- `requestVoteKick` pede ao servidor iniciar uma votacao contra o ID; `true`
  significa pedido enviado, nao votacao aprovada.
- `castVote` envia booleano `yes`; exige cliente conectado e votacao ativa no
  servidor. O servidor rejeita alvo, duplicata e voto invalido.
- Se aprovada, o servidor desconecta o alvo com motivo `"vote-kicked"`.

No servidor, `VoteKickStarted` recebe `(session, targetId, callerId)`. Nos
clientes, os eventos de estado recebem uma tabela de mensagem como segundo
argumento depois da sessao:

| Evento | Conteudo da mensagem no cliente | Contexto |
| --- | --- | --- |
| `VoteKickUpdated` | `{ targetId, yes, no, total, endsAt }` | Contagem atual; `total` e o numero de votantes elegiveis. |
| `VoteKickFinished` | `{ targetId, success, reason }` | Resultado final. |

`VoteKickStarted` e emitido localmente pelo servidor; o protocolo atual nao
envia um evento separado de inicio. O primeiro `VoteKickUpdated` recebido
indica que a votacao esta ativa.

O sistema nao substitui moderacao, autenticacao ou regras da comunidade.
