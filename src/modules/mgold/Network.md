# Multiplayer mgold

> **Indice:** esta pagina reune arquitetura e notas gerais. As assinaturas
> curtas aqui sao um mapa da API, nao substituem a referencia de argumentos,
> retornos e exemplos. Consulte os guias detalhados:
>
> - [Criacao, opcoes, ciclo e eventos](./Network-API.md)
> - [Players, dados e estado](./Network-Players.md)
> - [Scoreboard: formato, colunas e atualizacao dos valores](./Network-Scoreboard.md)
> - [Entidades, factories, fisica e mapa](./Network-Entities.md)
> - [Chat, comandos, RPC e input](./Network-Messaging.md)
> - [Times, modos, voz e vote kick](./Network-Gameplay.md)

Este documento descreve a API Lua `love.mgold.multiplayer`, exposta tambem
pelos atalhos `love.mgold.server`, `love.mgold.client`,
`love.mgold.gameMode`, `love.mgold.terminal` e `love.mgold.shared`. O helper
de scoreboard fica em `love.mgold.multiplayer.scoreboard`. O transporte usa
ENet; os pacotes da aplicacao sao
serializados por `love.mgold.tab`.

O framework cuida do transporte, handshake, estado de players e entidades,
replicacao de mapa, regras basicas de validacao e roteamento das mensagens.
Renderizacao de entidades e UI, controles do jogo, captura de voz e a escolha
de codec continuam sob responsabilidade do jogo.

## Arquivos do subsistema

Os fragmentos Lua abaixo sao concatenados e carregados por `Network.cpp`.
Mantenha a ordem dos includes do C++ e os arquivos listados no `CMakeLists.txt`
consistentes quando modificar ou adicionar fragmentos.

| Arquivo | Responsabilidade |
| --- | --- |
| `NetCore.lua` | Inicializacao, validacao/serializacao, estado de entidades e integracao com fisica. |
| `NetSession.lua` | Criacao e ciclo da sessao, handshake, entidades e colisores estaticos do mapa. |
| `NetUpdate.lua` | Atualizacao por frame, servico ENet, fisica e interpolacao. |
| `NetPlayers.lua` | Players, dados privados/publicos, scoreboard e hooks. |
| `NetPlayerSync.lua` | Validacao e correcao do estado dos players. |
| `NetMessaging.lua` | Chat, comandos, RPC/input e dados networked das entidades. |
| `NetTeamsModesVoice.lua` | Times, modos de jogo, lobby e infraestrutura de voz. |
| `NetModeration.lua` | Configuracao, rate limits, validacoes de pacote e vote kick. |
| `NetBootstrap.lua` | Builders `server`/`client`, terminal local e exportacao da API. |

`Network.cpp` incorpora os fragmentos no inicializador; nao carregue esses
arquivos Lua individualmente com `require`.

## Modelo de execucao

- Uma sessao multiplayer ativa por processo. Nao crie simultaneamente uma
  sessao 2D e uma 3D no mesmo processo.
- Servidor e clientes devem usar o mesmo modo de fisica: `"2d"` ou `"3d"`.
- Chame `session:update(dt)` uma vez por frame. A chamada atende eventos ENet,
  avanca a fisica selecionada e processa sincronizacao. Nao atualize a mesma
  world de Box2D em outro lugar.
- `client.ready` fica `false` ate o `welcome`; comandos e chat remotos so devem
  ser apresentados como disponiveis depois do evento `Connected`.
- O servidor e autoridade para mapa, criacao/remocao de entidades, dados de
  player, times, comandos remotos, votacao e estado replicado.
- Uma entidade `"client"` tem um cliente proprietario que simula localmente;
  o servidor valida os estados recebidos. Entidades `"server"` sao simuladas
  pelo servidor. Entidades `"static"` sao replicadas sem corpo de fisica.
- Os identificadores de player sao atribuidos pelo servidor durante o
  handshake; nao use nomes de jogador como identificadores estaveis.

## Cheat sheet: namespaces e assinaturas

Nas assinaturas abaixo, `:` representa uma chamada de metodo Lua e omite o
argumento `self`. `Session` pode ser servidor ou cliente; as restricoes de
autoridade estao indicadas em cada tabela. Salvo indicacao contraria,
operacoes que alteram estado retornam `self` para encadeamento. Uma chamada
invalida geralmente gera erro Lua; retornos `false` indicam uma recusa normal,
como ID inexistente, limite atingido ou cliente ainda desconectado.

### Namespaces exportados

| Assinatura | Resultado / uso |
| --- | --- |
| `love.mgold.multiplayer` | Tabela completa da API. |
| `love.mgold.network` | Alias da mesma tabela `multiplayer`. |
| `love.mgold.multiplayer.gameModes[name]` | Modo registrado; inclui os modos predefinidos. |
| `love.mgold.server.create(options?)` | `ServerBuilder`; chame `:start()`. |
| `love.mgold.client.create(options?)` | `ClientBuilder`; chame `:connect(address, port?)`. |
| `love.mgold.multiplayer.newServer(host, options?)` | `Session` de servidor com host ENet existente. |
| `love.mgold.multiplayer.newClient(host, peer?, options?)` | `Session` de cliente com host/peer ENet existentes. |
| `love.mgold.multiplayer.getActiveMode()` | `"2d"`, `"3d"` ou `nil`. |
| `love.mgold.gameMode.create(name, callbacks?)` | Tabela de modo pronta para `setGameMode`. |
| `love.mgold.multiplayer.scoreboard.create(columns?)` | Helper para desenhar uma tabela customizada; o scoreboard padrao da engine ja e automatico no cliente. |
| `love.mgold.terminal.create(client, options?)` | Buffer de texto ligado a eventos de resultado de comando. |
| `love.mgold.multiplayer.hook.add(name, callback)` | ID numerico do hook global. |
| `love.mgold.multiplayer.hook.remove(name, id)` | `boolean`; remove o hook global. |
| `love.mgold.multiplayer.hook.run(name, ...)` | Executa hooks globais; retorna `true` se callback retornar `true`. |
| `love.mgold.shared` | Alias de `multiplayer.shared`: `hook`, `gameMode`, `scoreboard` e `terminal`. |

`server.create` aceita uma tabela e aplica `port = 27015` e `maxPlayers = 16`
quando ausentes. `client.create` aceita `nil` ou tabela. O builder e uma
tabela Lua que delega os metodos da sessao depois de iniciado/conectado.

### Metodos comuns de Session

| Assinatura | Retorno | Observacoes |
| --- | --- | --- |
| `session:update(dt)` | `nil` | Obrigatorio uma vez por frame; `dt` finito e nao negativo. |
| `session:close()` | `boolean` | Fecha sessao e libera entidades, peers e colisores; builders tambem destroem o host. |
| `session:on(event, callback)` | ID numerico | Inscreve callback local a sessao. |
| `session:off(event, id)` | `boolean` | Remove callback local. |
| `session:getPlayers()` | Array de `Player` | Novo array com objetos gerenciados pela sessao. |
| `session:getPlayer(id)` | `Player` ou `nil` | Busca por ID atribuido pelo servidor. |
| `session:getScoreboard()` | Lista ordenada de linhas `{{ chave, valor }, ...}` | Somente dados publicos; campos padrao `name` e `color`. |
| `session:setScoreboardColumns(columns)` | `Session` | Define prioridade de ordem das chaves; outros campos publicos ainda aparecem. |
| `session:setScoreboardEnabled(enabled)` | `Session` | Liga/desliga a UI automatica do cliente; Tab mostra a tabela por padrao. |
| `session:isScoreboardEnabled()` | `boolean` | Informa se a UI automatica esta habilitada. |
| `session:getTeams()` | Tabela de snapshots por nome | Registro com `name`, `score` e `maxPlayers`. |
| `session:registerType(name, factory)` | `Session` | Registra fabrica local; deve existir em cada peer. |

### Metodos de servidor

| Assinatura | Retorno | Funcao |
| --- | --- | --- |
| `server:createEntity(kind, config)` | `Entity` | Cria e replica uma entidade. |
| `server:removeEntity(id)` | `boolean` | Remove entidade pelo ID. |
| `server:addStaticCollider(id, definition)` | ID | Registra colisor de mapa e o replica. |
| `server:removeStaticCollider(id)` | `boolean` | Remove colisor de mapa. |
| `server:removePlayer(id, reason?)` | `boolean` | Desconecta jogador. |
| `server:createTeam(name, options?)` | `Team` | Cria time; `options.maxPlayers` limita membros. |
| `server:setPlayerTeam(playerId, teamName?)` | `boolean` | Atribui ou remove time (`nil`). |
| `server:setGameMode(modeOrName)` | `Session` | Seleciona modo antes/durante lobby. |
| `server:spawnPlayer(playerId)` | `boolean` | Emite notificacao; nao cria personagem do jogo. |
| `server:playerDied(playerId, killerId?)` | `boolean` | Emite morte para modo/hooks; nao calcula dano. |
| `server:startGame()` | `boolean` | Inicia; `false` se ja estiver jogando. |
| `server:returnToLobby()` | `true` | Finaliza partida e muda o estado para lobby. |
| `server:registerCommand(name, callback, options?)` | `Session` | Registra comando local/remoto. |
| `server:runCommand(line)` | `success, result` | Executa localmente sem player remoto. |
| `server:sendMessage(text, targetId?)` | `boolean` | Envia chat de sistema global ou privado. |
| `server:registerRPC(name, callback, options?)` | `Session` | Handler de RPC cliente-servidor. |
| `server:registerInput(name, callback, options?)` | `Session` | Handler de input frequente do cliente. |
| `server:startVoteKick(targetId, callerId?)` | `success, reason?` | API administrativa local; requer vote kick habilitado. |

`removeEntity` nao faz verificacao explicita de role no metodo interno; use-o
no servidor para a remocao autoritativa. Chamado no cliente, afeta somente a
copia local e nao notifica o servidor.

### Metodos de cliente

| Assinatura | Retorno | Funcao |
| --- | --- | --- |
| `client:setLocalPlayerState(state)` | `Session` | Atualiza o estado local; estados alterados sao enviados ao servidor com limite de taxa. |
| `client:chat(text)` | `boolean` | Envia mensagem global. |
| `client:teamChat(text)` | `boolean` | Envia mensagem ao time. |
| `client:privateChat(targetId, text)` | `boolean` | Envia mensagem privada. |
| `client:runCommand(line)` | `success, reason?` | Enfileira comando no servidor; requer conexao pronta. |
| `client:sendRPC(name, payload)` | `boolean` | Envia RPC confiavel ao servidor. |
| `client:sendInput(name, payload)` | `boolean` | Envia input limitado por taxa. |
| `client:requestVoteKick(targetId)` | `boolean` | Solicita inicio de votacao. |
| `client:castVote(yes)` | `boolean` | Registra voto booleano. |
| `client:sendVoice(frame, channel?)` | `boolean` | Envia frame codificado; pode recusar se desativado/limitado. |
| `client:setVoiceCodec(encode?, decode?)` | `Session` | Define transformacoes locais dos frames. |
| `client:setPushToTalk(active)` | `Session` | Atualiza estado PTT local. |
| `client:mutePlayer(id)` / `unmutePlayer(id)` | `boolean` | Muta/desmuta voz local. |
| `client:muteAll()` / `unmuteAll()` | `Session` | Silencia/restaura toda voz local. |
| `client:setPlayerVoiceVolume(id, volume)` | `Session` | Define volume local entre 0 e 1. |
| `client:getPlayerVoiceVolume(id)` | Numero | Volume local; padrao 1. |

### Metodos de Player, Entity, Team e helpers

| Objeto | Assinatura | Retorno / efeito |
| --- | --- | --- |
| `Player` | `player:getData(key?)` | Copia dos dados visiveis; sem chave retorna a tabela toda. |
| `Player` | `player:setData(key, value, options?)` | Servidor; `options.public == true` publica para todos. |
| `Player` | `player:setTeam(name?)` | Delega a atribuicao ao servidor. |
| `Player` | `player:getState()` | Copia `{ position, rotation, velocity }`. |
| `Player` | `player:setState(state)` / `setPosition(x,y,z)` | Servidor corrige estado e inicia grace period. |
| `Player` | `player:getPing()` | RTT em ms no servidor; `0` se nao ha peer. |
| `Player` | `player:sendRPC(name, payload)` | Servidor envia RPC a esse jogador. |
| `Entity` | `entity:getState()` | Estado atual como copia. |
| `Entity` | `entity:setState(state)` / `setPosition(x,y,z)` | Aplica estado; servidor envia correcao confiavel. |
| `Entity` | `entity:destroy()` | Delega a remocao a sessao dona. |
| `Entity` | `entity:getNetworked(key)` | Le copia de dado networked ou `nil`. |
| `Entity` | `entity:setNetworked(key,value,options?)` | Servidor; `public=true` replica a todos. |
| `Team` | `team:setScore(score)` | Servidor atualiza e replica pontuacao. |
| `Team` | `team:getPlayers()` | Array de players ainda conectados. |
| `Scoreboard` | `board:setColumns(columns)` | Atualiza colunas do helper. |
| `Scoreboard` | `board:getRows(session)` | Retorna a lista `session:getScoreboard()`. |
| `Scoreboard` | `board:draw(session)` | Desenha manualmente os campos publicos enquanto Tab estiver pressionado. |
| `Terminal` | `terminal:submit(line)` | Escreve prompt e envia comando via cliente. |
| `Terminal` | `terminal:write(text)` | Acrescenta linhas, aparando ao limite configurado. |
| `Terminal` | `terminal:getLines()` | Copia da lista de linhas. |
| `Terminal` | `terminal:clear()` | Limpa o buffer. |

Retornos/callbacks sao descritos com mais contexto nas secoes seguintes.
Metodos prefixados com `_` (por exemplo, `_receive` e `_send`) sao detalhes
internos do protocolo, nao parte da API de jogo.

## Metatables, tabelas e userdata

A maioria dos objetos do framework nao e userdata. Sao tabelas Lua com
metatables internas:

| Valor | `type(value)` | Metatable / comportamento |
| --- | --- | --- |
| `Session` | `"table"` | `sessionMT.__index = sessionMT`; guarda estado, mapas, players, hooks e host. |
| `ServerBuilder` / `ClientBuilder` | `"table"` | `builderMT` customiza `__index` e `__newindex` para delegar a sessao. |
| `Player` | `"table"` | `playerMT.__index = playerMT`; registro gerenciado pelo servidor/snapshot. |
| `Entity` | `"table"` | `entityMT.__index = entityMT`; guarda autoridade, estado e referencias de fisica/modelo. |
| `Team` no servidor | `"table"` | `teamMT.__index = teamMT`; metodos de pontuacao e consulta de jogadores. |
| Time recebido no cliente | `"table"` | Snapshot simples; nao e instancia de `teamMT`. |
| Game mode | `"table"` | Tabela de callbacks e `name`; sem metatable especial. |
| Scoreboard helper | `"table"` | Tabela com closures `setColumns`, `getRows` e `draw`. |
| Terminal | `"table"` | Tabela com closures e buffer `lines`; sem metatable especial. |

Os metatables de Session/Player/Entity/Team sao variaveis locais da factory do
modulo, nao classes exportadas. `getmetatable(obj)` pode revelar a tabela em
runtime, mas modifica-la pode quebrar todas as instancias do tipo e nao e API
suportada. Nao compare metatables para identificar tipos; prefira campos
documentados e `type`.

### Userdata nativos e ownership

- **`enet_host` userdata:** devolvido por `enet.host_create`. E o transporte
  local; o builder guarda o host em `builder.host` e a sessao em `session.host`.
- **`enet_peer` userdata:** devolvido por `host:connect` e exposto como
  `player.peer` no servidor ou `session.peer` no cliente. Identifica uma
  conexao, nao um ID de player serializavel.
- **Box2D:** no modo 2D, `world`, `Body`, `Fixture` e `Shape` sao objetos
  nativos da API `love.physics` (userdata). A world e passada em `options.world`;
  o framework usa `world:update(dt)`, `body:setPosition(x,y)`,
  `body:getPosition()`, `body:setAngle(angle)`,
  `body:setLinearVelocity(vx,vy)`, `body:setType(type)`, `body:destroy()` e
  `body:isDestroyed()`.
- **Fisica mgold 3D:** `love.mgold.physics.newBody(...)` cria uma tabela Lua
  (`newBody(x,y,z,radius?,shape?)`) com metatable `love.mgold.physics.Body`;
  nao e userdata. Tem metodos
  `getPosition`, `setPosition`, `getVelocity`, `setVelocity`, `collisionTest`,
  `moveAndSlide`, `updatePhysics`, `setStatic`, `unsetStatic` e `destroy`.
- **Assinaturas do body 3D:** `getPosition()` e `getVelocity()` retornam
  `x, y, z`; `setPosition(x, y, z)` e `setVelocity(x, y, z)` alteram vetores;
  `collisionTest(displacementVector)` retorna, em colisao, `distance, point, normal,
  model` e, sem colisao, `nil` seguido por vetores zero; `moveAndSlide(delta)`
  retorna posicao e velocidade como dois vetores; `updatePhysics(dt, gravity?,
  maxFall?)`, `setStatic()`, `unsetStatic()` e `destroy()` alteram o body.
- **`entity.model`:** objeto fornecido pelo jogo/fabrica. O framework nao o
  serializa; use callbacks `getState`/`setState` para adaptar o modelo.

ENet registra as metatables nativas `enet_host` e `enet_peer`. As funcoes
documentadas sao metodos C e validam o tipo de userdata; uma tabela Lua com
campos semelhantes nao substitui host/peer. Os metodos sao acessados pelo
`__index` da metatable, portanto use `peer:send(...)`, nao chame a funcao como
se fosse um metodo estatico.

Assinaturas ENet relevantes para a integracao de baixo nivel:

```lua
local enet = require("enet")
local host, err = enet.host_create(addressOrNil, peerCount, channelCount)
local peer = host:connect("host:port", channelCount, connectData)
local eventOrNil = host:service(timeoutMilliseconds)
host:flush()
host:destroy()

local result = peer:send(bytes, channelId, "reliable") -- ou "unreliable"
peer:disconnect(data)
local state = peer:state()
local roundTripMilliseconds = peer:round_trip_time()
```

`host_create` retorna `enet_host` userdata ou `nil, error`; `host:connect`
retorna `enet_peer` userdata. `host:service` retorna `nil` sem evento ou uma
tabela de evento: `type` e `peer`, mais `data` para connect/disconnect
(inteiro) ou receive (string). ENet tambem expoe `disconnect_now`,
`disconnect_later`, `ping`, `ping_interval`, `timeout`, `receive`,
`round_trip_time` e `last_round_trip_time` no peer. Os eventos sao drenados
automaticamente por `session:update`; nao chame `host:service` em paralelo
com uma sessao ativa.

Um player remoto no servidor tem `player.peer` como ENet userdata. O espelho
do player no cliente nao expoe o peer do servidor e usa `peer = nil`. Objetos
ENet, bodies, fixtures, callbacks e metatables nunca devem ser colocados em
`data`, `networkData`, perfil ou payload RPC/input; o serializador aceita
somente os tipos Lua simples descritos na secao de opcoes.

### Campos dos objetos

Os campos abaixo sao acessiveis como tabelas Lua, mas a escrita direta nao
dispara validacao, eventos ou replicacao. Prefira os metodos da cheat sheet.

| Objeto | Campos mais usados |
| --- | --- |
| `Session` | `role`, `mode`, `state`, `ready`, `closed`, `error`, `host`, `peer`, `playerId`, `player`, `options`, `mapRevision`, `maxPlayers`, `playerCount`. |
| `Player` | `id`, `peer`, `data`, `visibility`, `state`, `team`, `connected`, `body`. |
| `Entity` | `id`, `kind`, `simulation`, `state`, `data`, `networkData`, `networkVisibility`, `physicsConfig`, `body`, `model`, `owner`, `localOwner`. |
| `Team` do servidor | `name`, `score`, `maxPlayers`, `players`, `manager`. |
| `Builder` | `kind`, `options`, `host`, `_session`. |
| `Terminal` | `client`, `lines`, `maxLines`. |

`session.entities`, `session.playersById` e outros mapas internos sao
implementacao, nao pontos de extensao suportados. `player.data.score = ...`
ou `entity.networkData.key = ...` altera somente a tabela local; use
`setData`/`setNetworked` para executar o fluxo correto. `getData`,
`getNetworked`, `getState`, `getScoreboard` e `getLines` retornam copias onde
indicado, portanto editar a copia tambem nao muda o objeto original.

O builder possui campos proprios `kind`, `options`, `host` e `_session`.
Depois de iniciado, a leitura de outros campos consulta a sessao; a escrita
para um campo que ja existe na sessao e encaminhada para ela. `start`,
`connect` e `close` sao metodos do builder e prevalecem sobre nomes delegados.
O campo `_session` e interno; nao o altere diretamente.

## Criar e encerrar sessoes

Os builders carregam `enet` quando necessario. O servidor usa a porta 27015
por padrao e escuta em todas as interfaces. O cliente conecta ao endereco e
porta fornecidos. Para Box2D, cada processo deve fornecer sua propria world:

```lua
local server = love.mgold.server.create({
    port = 22122,
    maxPlayers = 16,
    physics = "2d",
    world = love.physics.newWorld(0, 9.81, true),
    chat = { enabled = true },
    voice = { enabled = true, team = true },
    voteKick = { enabled = true, requiredVotes = 0.6, duration = 30 },
}):start()

function love.update(dt)
    server:update(dt)
end

function love.quit()
    server:close()
end
```

```lua
local client = love.mgold.client.create({
    physics = "2d",
    world = clientWorld,
    profile = { name = "Jogador" },
    getLocalPlayerState = function()
        -- Funcao de exemplo fornecida pelo proprio jogo.
        return player:getNetworkState()
    end,
}):connect("127.0.0.1", 22122)

function love.update(dt)
    client:update(dt)
end

function love.quit()
    client:close()
end
```

O nome enviado no perfil e `options.profile.name` (ate 32 caracteres sem
caracteres de controle). Se omitido ou invalido, o servidor cria um nome
`PlayerN`. A configuracao `physics` do cliente e do servidor precisa coincidir;
um modo divergente e rejeitado.

As chamadas acima criam builders. `:start()` inicializa o servidor;
`:connect(address, port)` conecta o cliente. Os builders delegam os metodos da
sessao depois de inicializados, como `update`, `on`, `getPlayers` e
`createEntity`. `:close()` no builder fecha a sessao e destroi o host ENet.
Para a API de baixo nivel, use `love.mgold.multiplayer.newServer(host, opts)`
ou `newClient(host, peer, opts)` com objetos ENet ja criados; nesse caso, o
codigo que criou o host continua responsavel por destrui-lo. `getActiveMode()`
retorna o modo ativo (`"2d"`, `"3d"` ou `nil`).

```lua
local enet = require("enet")
local host, err = enet.host_create("*:22122", 16, 1)
if not host then error(err) end

local session = love.mgold.multiplayer.newServer(host, {
    physics = "3d",
})

function love.update(dt)
    session:update(dt)
end

function love.quit()
    session:close()
    host:destroy()
end
```

## Opcoes

| Opcao | Padrao | Descricao |
| --- | --- | --- |
| `port` | `27015` no builder | Porta local do servidor; intervalo 1-65535. |
| `maxPlayers` | `16` | Limite de jogadores no servidor. |
| `physics` | `"3d"` | Modo de fisica compartilhado, `"2d"` ou `"3d"`. |
| `world` | sem padrao | Obrigatoria em `"2d"`; world Box2D deste processo. |
| `profile` | `{}` | Perfil do cliente; somente `profile.name` e transmitido pelo handshake. |
| `maxPacketBytes` | `65536` | Tamanho maximo aceito por pacote; minimo 1024 bytes. |
| `maxCommandLength` | `256` | Comprimento maximo de um comando. |
| `interpolationDelay` | `0.1` s | Atraso aplicado a interpolacao de entidades e jogadores remotos no cliente. |
| `stateEpsilon` | `0.001` | Diferenca minima para replicar estado de entidade alterado. |
| `validationInterval` | `0.25` s | Intervalo de solicitacao de estado para entidades client-authoritative. |
| `playerValidationInterval` | `0.25` s | Intervalo minimo entre estados alterados enviados pelo cliente; maximo efetivo de 8 atualizacoes/s. |
| `maxPlayerSpeed` | `20` | Velocidade maxima usada pela validacao automatica de player em 2D/3D. |
| `positionTolerance` | `0.5` | Margem de movimento na validacao automatica de player. |
| `syncGrace` | `0.25` s | Periodo de tolerancia apos uma correcao. |
| `initialPlayerState` | origem | Estado inicial opcional para novos players. |
| `playerBody` | desativado | Definicao opcional do corpo fisico do player no servidor. |
| `createPlayerData` | sem callback | Callback do servidor para dados iniciais adicionais de player. |
| `validatePlayerMotion` | sem callback | Validador do servidor para estado recebido de player. |
| `getLocalPlayerState` | sem callback | Callback do cliente para responder a solicitacoes de estado. |
| `onPositionCorrection` | sem callback | Callback do cliente quando recebe correcao de player. |
| `validateMotion` | sem callback | Validador geral para estado recebido de entidade client-authoritative. |
| `chat` | descrito abaixo | Opcoes e filtro do chat. |
| `voice` | descrito abaixo | Opcoes de transporte e roteamento de voz. |
| `voteKick` | descrito abaixo | Configuracao da votacao para expulsar jogador. |
| `gameMode` | sem modo | Modo inicial do servidor, por nome ou objeto criado por `gameMode.create`. |

Callbacks e opcoes sao locais ao processo; somente os campos explicitamente
serializados sao transmitidos. Numeros transmitidos devem ser finitos. Os dados
de rede aceitam `nil`, booleanos, strings, numeros finitos e tabelas sem ciclos,
com chaves string, number ou boolean; valores com funcao, userdata, thread ou
ciclo nao podem ser enviados. A profundidade maxima das tabelas e 64.

## Atualizacao, conexao e eventos

O cliente passa por handshake e fica pronto depois de receber `welcome`.
Eventos uteis incluem `Connected`, `Disconnected`, `GameStateChanged`,
`PlayersSnapshot`, `PlayerJoin`, `PlayerLeave`, `PlayerDataChanged`,
`EntityCreated`, `EntityRemoved`, `EntityDataChanged`, `ChatMessageReceived`,
`CommandResult`, `VoteKickUpdated` e `VoteKickFinished`.

Inscreva-se com `session:on(event, callback)`; o callback recebe a sessao como
primeiro argumento. Remova a inscricao com `session:off(event, id)`. Exemplos:

```lua
local joinHook = server:on("PlayerJoin", function(session, player)
    print(player.id, player.data.name, "entrou")
end)

server:off("PlayerJoin", joinHook)

local globalHook = love.mgold.multiplayer.hook.add("PlayerLeave", function(session, player, reason)
    print(player.data.name, reason)
end)
love.mgold.multiplayer.hook.remove("PlayerLeave", globalHook)
```

Hooks globais recebem `(session, ...)`; hooks da sessao recebem os mesmos
argumentos. Modos podem implementar callbacks `onPlayerJoin`,
`onPlayerLeave`, `onPlayerState`, `onPlayerPositionCorrected`,
`onPlayerTeamChanged`, `onReturnToLobby`, `onPlayerSpawn` e `onPlayerDeath`.
Callbacks de modo recebem o objeto de modo como primeiro argumento.

### Catalogo de eventos e argumentos

Para `session:on("Event", callback)`, os argumentos abaixo aparecem depois do
primeiro argumento `session`. O hook global `multiplayer.hook.add` recebe a
mesma lista, tambem precedida pela sessao.

| Evento | Lado / argumentos | Quando ocorre |
| --- | --- | --- |
| `PeerConnected` | Servidor: `peer` (`enet_peer` userdata) | ENet conectou; antes do handshake de player. |
| `Connected` | Cliente: `playerId, player` | Handshake e snapshot de players recebidos. |
| `Disconnected` | Cliente: sem argumentos adicionais | Peer do servidor desconectou. |
| `PlayerJoin` | Servidor/cliente: `player` | Player adicionado a lista local. |
| `PlayerLeave` | Servidor/cliente: `player, reason` | Player removido da lista. |
| `PlayersSnapshot` | Cliente: `players` | Snapshot inicial/substituido da lista. |
| `PlayerDataChanged` | Cliente: `player` | Dados/snapshot daquele player atualizados. |
| `PlayerState` | Servidor/cliente: `player, state` | Estado aceito/recebido sem marca de correcao. |
| `PlayerPositionCorrected` | Servidor/cliente: `player, state` | Estado corrigido pela autoridade. |
| `InvalidPlayerMovement` | Servidor: `player, proposedState` | Movimento rejeitado pelo validador. |
| `EntityCreated` / `EntityRemoved` | Ambos: `entity` | Entidade criada ou removida localmente. |
| `EntityDataChanged` | Cliente: `entity, key, value` | Campo networked atualizado; remocao usa `value = nil`. |
| `TeamCreated` | Servidor: `team` | Time criado. |
| `TeamScoreChanged` | Servidor: `team, score` | Pontuacao do time alterada. |
| `TeamsChanged` | Cliente: `teams` | Snapshot de times substituido. |
| `PlayerTeamChanged` | Servidor: `player, teamName` | Atribuicao de time alterada; `teamName` pode ser `nil`. |
| `GameModeChanged` | Servidor: `mode` | Modo selecionado. |
| `GameStart` | Servidor: `mode` ou `nil` | Partida iniciada. |
| `GameStateChanged` | Cliente: `state, modeName` | Estado/mode replicado pelo servidor. |
| `PlayerSpawn` | Servidor: `player` | Jogo notificou spawn. |
| `PlayerDeath` | Servidor: `player, killer` | Jogo notificou morte; killer pode ser `nil`. |
| `ReturnToLobby` | Servidor: `mode` ou `nil` | Partida encerrada/retorno ao lobby. |
| `ChatMessage` | Servidor: `player, channel, text, target` | Chat validado, antes/depois do roteamento. |
| `ChatMessageReceived` | Cliente: `message` | Mensagem de chat entregue; objeto contem canal, remetente, texto, hora. |
| `CommandResult` | Cliente: `success, message` | Resposta a comando remoto. |
| `CommandError` | Servidor: `player, commandName, error` | Callback de comando local falhou. |
| `InvalidCommand` | Servidor: `player, commandName` | Nome de comando desconhecido. |
| `VoicePacketReceived` | Servidor: `sender, encodedFrame, channel` | Frame de voz recebido antes do roteamento. |
| `VoicePacket` | Cliente: `senderId, decodedFrame, channel, volume` | Frame recebido e nao mutado. |
| `VoteKickStarted` | Servidor: `targetId, callerId` | Votacao criada. |
| `VoteKickUpdated` | Servidor: `targetId, yes, no, total`; cliente: `voteState` | Contagem ou snapshot de votacao mudou. |
| `VoteKickFinished` | Servidor: `targetId, success, reason`; cliente: `voteResult` | Votacao terminou. |
| `RateLimitExceeded` | Servidor: `playerOrNil, category` | Limite de mensagens excedido. |
| `ProtocolViolation` | Servidor: `peer, reason` | Pacote, payload ou tipo de mensagem invalido. |
| `Update` | Servidor: `dt` | Uma vez por `update(dt)` no lado servidor. |

## Mapa e colisores estaticos

Somente o servidor registra os colisores compartilhados. Cada adicao ou
remocao incrementa a revisao e e enviada de forma confiavel aos clientes
conectados; novos clientes recebem o mapa registrado no handshake. Nao ha
replicacao por frame nem atualizacao in-place: remova e registre novamente um
colisor para substitui-lo.

```lua
server:addStaticCollider("chao", {
    shape = "rectangle",
    position = { x = 0, y = 10 },
    width = 40,
    height = 1,
})

server:removeStaticCollider("chao")
```

No modo 2D, use:

- `shape = "circle"` com `radius` e `position`;
- `shape = "rectangle"` com `width`, `height`, `position` e angulo opcional
  `angle`;
- `shape = "polygon"` com `points`, uma lista plana de coordenadas locais.

No modo 3D, a definicao requer `vertices`: lista plana de coordenadas que
descreve triangulos locais (pelo menos nove numeros, em multiplos de tres).
`position`, `rotation`, `scale` e `zoneSize` sao opcionais. A representacao e
construida como modelo estatico da fisica mgold e compartilhada por todos os
clientes.

Esses colisores sao estaticos do mapa, nao entidades. Nao sao incluidos em
`getPlayers()` nem atualizados por estado de entidade.

## Entidades e autoridade da simulacao

O servidor cria entidades por `createEntity(kind, config)`. Se `simulation`
nao for informada, o padrao e `"server"`.

| `simulation` | Autoridade | Replicacao |
| --- | --- | --- |
| `"server"` | Servidor simula corpo e estado. | Estado alterado e transmitido; cliente interpola. |
| `"client"` | Cliente proprietario simula localmente. | Servidor solicita estados, valida e retransmite ou corrige. |
| `"static"` | Nenhuma simulacao fisica. | Enviada ao entrar/mudar; sem corpo fisico. |

Exemplo de entidade server-authoritative:

```lua
local grenade = server:createEntity("grenade", {
    simulation = "server",
    physics = { shape = "sphere", radius = 0.2 },
    position = { x = 0, y = 2, z = 0 },
    velocity = { x = 1, y = 0, z = 0 },
    data = { fuse = 3 },
})
```

Os estados usam vetores `position`, `rotation` e `velocity`, cada um com
componentes `x`, `y`, `z` (ou indices 1, 2, 3 ao serem lidos). Cada componente
deve ser numerico e finito. Pode-se fornecer `state = { position = ..., rotation
= ..., velocity = ... }` ou os campos de nivel superior `position`, `rotation`
e `velocity`.

Metodos de entidade:

- `entity:getState()` le o estado atual do corpo/callback;
- `entity:setState(state)` aplica um novo estado; no servidor envia correcao
  confiavel e inicia o periodo de tolerancia;
- `entity:setPosition(x, y, z)` altera posicao e preserva os demais campos;
- `entity:destroy()` remove a entidade e notifica os clientes;
- `entity:getNetworked(key)` e `entity:setNetworked(key, value, options)`
  consultam ou atualizam dados networked.

Para entidade client-authoritative, informe o proprietario como
`owner = player.peer` na criacao do lado servidor. Somente esse peer pode
responder as solicitacoes de estado. `validateMotion(entity, proposed, peer,
mode)` pode retornar `true`, `false` ou um estado corrigido valido; o mesmo
validador pode ser definido na configuracao da fabrica do tipo. Em 2D, a
validacao de movimento client-authoritative exige callback. Em 3D, ha uma
checagem de colisao basica quando a entidade tem corpo.

### Fabrica de tipo

`registerType(name, factory)` registra uma fabrica localmente. Registre a
fabrica no servidor e nos clientes antes de criar/receber entidades daquele
tipo. A fabrica recebe `(data, initialState, context)`, podendo retornar
`body`, `model` e callbacks:

```lua
local function registerProp(session)
    session:registerType("prop", function(data, state, context)
        local object = makeProp(data, state, context)
        return {
            model = object,
            getState = function(self, entity)
                return self.model:getState()
            end,
            setState = function(self, nextState, entity)
                self.model:setState(nextState)
            end,
            destroy = function(self, entity)
                self.model:destroy()
            end,
            validateMotion = function(self, proposed, peer, mode, entity)
                return validatePropMove(entity, proposed)
            end,
        }
    end)
end
```

`context` contem `role`, `mode`, `world`, `physics`, `entity` e `localOwner`.
Os metodos da instancia recebem a instancia como primeiro argumento; os
wrappers fornecem tambem a entidade conforme mostrado.

No modo 3D, os corpos de fisica de rede suportam `sphere` e `capsule`; a
fisica mgold atual testa colisao contra modelos estaticos, mas nao resolve
colisoes entre corpos dinamicos entre si. No modo 2D, corpos Box2D suportam
`circle`, `rectangle`/`box` e `polygon`.

## Players, dados e scoreboard

O servidor cria IDs e fornece campos iniciais publicos `name` e `color`.
`color` comeca em `{ 1, 1, 1, 1 }`; outras estatisticas sao opcionais e o jogo
as cria com `player:setData`.
Consulte `session:getPlayers()` e `session:getPlayer(id)`. O cliente tambem
recebe um retrato sincronizado da lista; `getPing()` e medido no peer ENet do
servidor.

O servidor controla dados de player:

```lua
player:setData("score", 10, { public = true })
player:setData("inventory", { "key", "potion" }) -- privado por padrao

local allOwnData = player:getData()
local score = player:getData("score")
```

Campos publicos sao enviados aos clientes; campos privados nunca sao enviados,
nem mesmo ao dono da linha. `createPlayerData(player, profile)` pode retornar
uma tabela com dados adicionais para cada novo player. Os dados iniciais
retornados sao privados por padrao; use `player:setData` para publicar
estatisticas. O servidor corrige estado de player com
`player:setState(state)` ou `player:setPosition(x, y, z)`.

O cliente envia o estado local quando ele muda, limitado por
`playerValidationInterval`. Atualize-o com `setLocalPlayerState(state)` ou
configure `getLocalPlayerState()` (callback sem argumentos). O estado inclui
posicao, rotacao/angulo e velocidade; o servidor valida e replica aos outros
clientes. O servidor pode usar `validatePlayerMotion(player, proposed, mode)`, retornando
`true`, `false` ou estado corrigido. Se ausente, ambos os modos limitam
velocidade e distancia; em 3D tambem verificam colisao estatica quando
possivel. `onPositionCorrection(player, state)` e executado no cliente
quando uma correcao de player chega.

`getScoreboard()` retorna uma lista de rows; cada row e um array ordenado de
pares `{ chave, valor }`. `name` e `color` vem primeiro por padrao; os demais
campos publicos sao opcionais e ordenados deterministicamente. A UI incluida
no helper e desenhada por `board:draw(session)` enquanto Tab estiver
pressionado. Consulte [Network-Scoreboard.md](./Network-Scoreboard.md) para
exemplos e detalhes de privacidade.

## Times, modos e lobby

No servidor, `createTeam(name, { maxPlayers = n })` cria um time;
`setPlayerTeam(playerId, name)` atribui ou remove o time (use `nil` para
remover); `team:setScore(score)` altera a pontuacao e `team:getPlayers()`
retorna seus jogadores. `session:getTeams()` retorna uma tabela indexada pelo
nome do time. Times e pontos sao enviados aos clientes.

Crie modos com `love.mgold.gameMode.create(name, callbacks)`. Modos predefinidos:
`lobby`, `deathmatch` e `teamDeathmatch` (alias `"team-deathmatch"`). Modos customizados podem implementar os callbacks abaixo. Todos sao chamados
como metodos Lua: a propria tabela de modo e sempre o primeiro argumento.

- `onInitialize(mode, session)` ao selecionar;
- `onStart(mode, session)` e `onEnd(mode, session)` na transicao de partida;
- `onPlayerJoin(mode, player)`, `onPlayerLeave(mode, player, reason)`,
  `onPlayerState(mode, player, state)`,
  `onPlayerPositionCorrected(mode, player, state)`,
  `onPlayerTeamChanged(mode, player, team)`,
  `onPlayerSpawn(mode, player)` e
  `onPlayerDeath(mode, player, killer)`;
- `onReturnToLobby(mode, modeArgument)` ao retornar ao lobby;
- `update(mode, session, dt)` em cada atualizacao do servidor.

Atualmente, o argumento adicional de `onReturnToLobby` e a propria tabela de
modo (os dois argumentos sao a mesma referencia); o callback pode aceitar
somente `mode` se nao precisar do segundo parametro.

```lua
local survival = love.mgold.gameMode.create("survival", {
    onInitialize = function(mode, session)
        session.round = 0
    end,
    onPlayerDeath = function(mode, player, killer)
        if killer then
            killer:setData("score", (tonumber(killer.data.score) or 0) + 1, {
                public = true,
            })
        end
    end,
    update = function(mode, session, dt)
        -- Logica autoritativa especifica do jogo.
    end,
})

server:setGameMode(survival)
```

O estado inicial e `"lobby"`. O servidor seleciona modo com
`setGameMode(modeOrName)`, inicia com `startGame()` e retorna com
`returnToLobby()`. `spawnPlayer(id)` e `playerDied(id, killerId)` sao
notificacoes/operacoes do servidor; o jogo continua responsavel por criar
personagem, respawn, regras de pontuacao e condicoes de vitoria.

## Chat e comandos

O chat aceita canais `global`, `team`, `private` e mensagens de sistema
enviadas pelo servidor. Clientes usam `chat(text)`, `teamChat(text)` e
`privateChat(targetId, text)`. O servidor usa `sendMessage(text, targetId)`
para enviar mensagem de sistema a todos ou a um jogador. O evento
`ChatMessageReceived(message)` no cliente recebe um objeto com `channel`,
`senderId`, `senderName`, `targetId`, `text` e `time` conforme aplicavel.

Opcoes de `chat`:

| Campo | Padrao |
| --- | --- |
| `enabled` | `true` |
| `global` | `true` |
| `team` | `true` |
| `private` | `true` |
| `maxLength` | `256` |
| `filter` | nenhum |

`filter(text, player, channel, target)` deve retornar texto valido. Para
rejeitar, retorne valor que nao seja string, string vazia ou texto acima do
limite; a mensagem nao sera entregue.

Registre comandos no servidor com
`registerCommand(name, callback, options)`. O callback recebe
`(player, args, server)`. Comandos padrao: `status`, `say` e `kick`; `kick`
remoto exige que o jogador tenha `data.admin == true` ou `data.rank == "admin"`.
Opcoes de comando incluem `admin`, `serverOnly` e `permission(player, server)`.
Para clientes, `runCommand(line)` envia a requisicao; no servidor tambem pode
ser usado para executar localmente. As requisicoes remotas sao limitadas a
cinco por janela de cinco segundos por peer.

`love.mgold.terminal.create(client, { maxLines = 100 })` fornece `submit`,
`write`, `clear` e `getLines`. E apenas um buffer de texto com resposta de
comandos; o jogo deve implementar captura de teclado, apresentacao e
renderizacao.

## RPC e input

RPC sao mensagens nomeadas com payload serializavel, nao execucao de codigo
remoto. Registre a mesma identificacao nos lados apropriados:

```lua
-- Servidor recebe RPC do cliente.
server:registerRPC("buyItem", function(player, payload, session)
    if payload.item == "potion" then
        grantPotion(player)
    end
end, {
    permission = function(player, payload, session)
        return player.data.canShop == true
    end,
})

-- Cliente envia RPC.
client:sendRPC("buyItem", { item = "potion" })

-- Cliente registra callback para RPC enviado pelo servidor.
client:registerRPC("openDoor", function(payload, session)
    openDoorLocally(payload.doorId)
end)

-- Servidor envia ao jogador.
server:getPlayer(playerId):sendRPC("openDoor", { doorId = 12 })
```

`registerRPC` no servidor recebe `(player, payload, session)`; no cliente
recebe callback `(payload, session)`. Opcoes de RPC no servidor aceitam
`permission(player, payload, session)`, que deve retornar `true`. O servidor
limita RPCs recebidas a 20 por segundo por peer.

Para inputs frequentes do cliente, o servidor pode usar
`registerInput(name, callback, options)`; o callback recebe
`(player, payload, session)`, e o cliente envia com `sendInput(name, payload)`.
O input e limitado a 60 por segundo por peer. Ambos os tipos devem validar
permissoes, valores e transicoes de estado no servidor.

## Dados networked de entidade

`entity.data` e a carga inicial da entidade. Para atualizacoes posteriores,
use `entity:setNetworked(key, value, { public = true })`; a chave publica e
replicada a todos, enquanto dados nao publicos sao enviados somente ao
proprietario. O cliente le com `entity:getNetworked(key)` e recebe
`EntityDataChanged(entity, key, value)`. Dados de rede seguem os mesmos
limites de serializacao dos payloads de RPC.

## Infraestrutura de voz

O transporte de voz e opcional e vem desativado. Nao inclui captura de
microfone, codec, gerenciamento de permissao do sistema operacional nem
reproducao de audio. O jogo captura frames; se `encodeFrame` estiver definido,
`sendVoice` o chama antes de transmitir. Sem encoder, passe bytes ja codificados:

```lua
local client = love.mgold.client.create({
    voice = { enabled = true, pushToTalk = true },
}):connect("127.0.0.1", 22122)

client:setVoiceCodec(encodeFrame, decodeFrame)
client.voice.onPacket = function(senderId, frame, channel, volume)
    audioPlayer:play(senderId, frame, volume)
end

client:sendVoice(capturedFrame, "global")
```

No servidor, habilite a voz em `voice.enabled`. Campos de roteamento:

| Campo | Padrao | Funcao |
| --- | --- | --- |
| `enabled` | `false` | Habilita o transporte. |
| `global` | `true` | Aceita/roteia canal global. |
| `team` | `false` | Habilita canal de time. |
| `channels` | sem canais extras | Mapa `{ nome = true }` de canais customizados. |
| `maxPacketBytes` | `4096` | Maximo de bytes por frame. |
| `maxPacketsPerSecond` | `50` | Limite de frames por segundo por remetente. |
| `maxDistance` | `0` | Distancia maxima; zero significa ilimitada. |
| `pushToTalk` | `false` | Quando `true`, `sendVoice` so envia durante `setPushToTalk(true)`. |
| `canHear` | nenhum | Predicado de roteamento opcional. |

`canHear(sender, listener, channel, server, distance)` deve retornar
exatamente `true` para permitir. Sem callback, o canal team roteia apenas a
players do mesmo time; o global roteia para todos, respeitando `maxDistance`.
`setPushToTalk(bool)`, `mutePlayer(id)`, `unmutePlayer(id)`, `muteAll()`,
`unmuteAll()`, `setPlayerVoiceVolume(id, volume)` e
`getPlayerVoiceVolume(id)` sao controles locais do cliente. Volume e limitado
ao intervalo 0-1. O evento/callback recebe o frame apos decodificacao e o
volume local do remetente.

## Vote kick

Vote kick vem desativado por padrao. Configure no servidor:

```lua
voteKick = {
    enabled = true,
    duration = 30,
    requiredVotes = 0.6,
}
```

`requiredVotes` e uma fracao entre 0 e 1 da quantidade de votantes elegiveis,
arredondada para cima e com minimo de um voto. O alvo nao pode votar. Apenas
uma votacao pode estar ativa; votos duplicados nao sao aceitos. Cliente usa
`requestVoteKick(playerId)` para propor e `castVote(trueOrFalse)` para votar.
Eventos `VoteKickStarted`, `VoteKickUpdated` e `VoteKickFinished` informam
resultado. Uma votacao aprovada remove o player do servidor; nao e uma
substituicao de ferramentas de moderacao ou politicas de servidor.

## Autoridade, validacao e limites de seguranca

O servidor limita tamanho de pacote, serializa os dados permitidos e aplica
limites por tipo de mensagem: chat (6/5 s), comando (5/5 s), inicio de voto
(1/10 s), votos (2/5 s), estado de player (8/s), RPC (20/s), input (60/s) e
voz (configuravel, padrao 50/s). Excesso e reportado por
`RateLimitExceeded`; pacotes invalidos podem emitir `ProtocolViolation`.

Validacao de velocidade/colisao e uma protecao basica, nao um anti-cheat
completo. Nao aceite valores economicos, dano, inventario ou resultados de
partida vindos diretamente do cliente. Valide tudo que afeta outros jogadores
no servidor e configure `validatePlayerMotion`, `validateMotion` e callbacks
de permissao conforme as regras do jogo. O transporte nao fornece criptografia
de aplicacao, identidade autenticada ou persistencia de conta.

Estados de entidade que mudam sao enviados de forma nao confiavel para reduzir
latencia; criacao, remocao, mapa, dados, chat, comandos e correcoes usam
mensagens confiaveis. As mensagens usam o canal ENet 0. O jogo deve tolerar
perda de atualizacoes de estado e usar a interpolacao fornecida para entidades
remotas.

## Diagnostico e encerramento

- Se a conexao for rejeitada, confira `client.error`, o modo de fisica e se o
  servidor atingiu `maxPlayers`.
- Se os dados 2D nao forem simulados, confirme que cada sessao recebeu uma
  `world` correta e que `update(dt)` e chamado uma unica vez.
- Se um entity factory nao for chamado no cliente, registre o tipo no cliente
  antes do handshake/criacao da entidade.
- Se chat, voz ou votacao nao forem entregues, confira as opcoes no servidor,
  o estado de conexao e as permissoes/canais configurados.
- Chame `:close()` no builder durante o encerramento para liberar sessao, peers
  e host ENet. Entidades removidas e colisores registrados sao liberados pela
  sessao.
