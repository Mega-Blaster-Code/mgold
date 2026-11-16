# Referencia: criacao e ciclo da sessao

## Namespaces

O inicializador exporta:

| Expressao | O que e |
| --- | --- |
| `love.mgold.multiplayer` | API completa. |
| `love.mgold.network` | Alias da mesma tabela. |
| `love.mgold.server` / `.client` | Builders para ENet. |
| `love.mgold.gameMode` | Fabrica de modos; detalhes em Network-Gameplay. |
| `love.mgold.terminal` | Fabrica de buffer de comandos. |
| `love.mgold.shared` | Referencias a hook, gameMode, scoreboard e terminal. |

## Criar servidor

```lua
local server = love.mgold.server.create(options?) --> ServerBuilder
server:start() --> ServerBuilder
```

`options` e uma tabela; `nil` vale tabela vazia. O builder aplica `port = 27015`
e `maxPlayers = 16` quando ausentes.

- `port`: numero inteiro 1..65535. Porta na qual ENet escuta.
- `maxPlayers`: limite positivo; convertido para inteiro ao criar sessao.
- `physics`: `"2d"` ou `"3d"`, padrao `"3d"`.
- `world`: world `love.physics` obrigatoria em 2D.
- `maxPacketBytes`: limite de pacote, padrao 65536 e minimo 1024.
- `gameMode`: nome registrado ou tabela de modo para selecionar inicialmente.
- `chat`, `voice`, `voteKick`: tabelas de configuracao especificas descritas
  em [Network-Messaging.md](./Network-Messaging.md) e
  [Network-Gameplay.md](./Network-Gameplay.md).
- demais opcoes de validacao e callbacks: veja abaixo.

`start()` cria host ENet no endereco `"*:<port>"` e uma sessao local. Falha de
criacao de host gera erro Lua. O builder encaminha metodos e campos da sessao
depois de iniciado; portanto `server:update(dt)` e `server:close()` podem ser
chamados diretamente.

## Criar cliente

```lua
local client = love.mgold.client.create(options?) --> ClientBuilder
client:connect(address, port?) --> ClientBuilder
```

- `address`: string nao vazia, por exemplo `"127.0.0.1"` ou hostname.
- `port`: padrao `options.port` ou `27015`; inteiro 1..65535.
- `options.profile.name`: nome apresentado no player; string valida ate 32
  bytes, sem caracteres de controle. O protocolo atual transmite somente este
  campo do perfil.
- `physics`, `world` e opcoes de sincronizacao precisam ser compativeis com o
  servidor.

`connect` cria host ENet e inicia a conexao. O builder nao implica que o
handshake ja terminou: `client.ready` so fica `true` depois de `welcome`.
Use evento `Connected` para saber que o snapshot inicial de players chegou.

## Usar um host ENet existente

Se o jogo gerencia ENet por conta propria, pode criar uma sessao sem os
builders:

```lua
love.mgold.multiplayer.newServer(host, options?) --> Session de servidor
love.mgold.multiplayer.newClient(host, peer?, options?) --> Session de cliente
```

- `host`: userdata `enet_host` criado pelo modulo ENet; argumento obrigatorio.
- `peer`: userdata `enet_peer` do servidor no cliente, ou `nil` ate iniciar/
  receber a conexao.
- `options`: mesmas opcoes de sessao listadas abaixo.
- Retorna a sessao diretamente, sem builder. A conexao/servico continua a
  cargo do framework por `session:update(dt)`.
- Ha uma sessao multiplayer ativa por processo, mesmo alternando entre 2D e
  3D.
- `session:close()` desconecta e libera os objetos pertencentes a sessao, mas
  nao destroi o host ENet fornecido pelo chamador. Quem criou o host continua
  responsavel por `host:destroy()`.

## Opcoes compartilhadas da sessao

| Opcao | Padrao | Significado |
| --- | --- | --- |
| `physics` | `"3d"` | Modo local; cliente e servidor devem usar o mesmo. |
| `world` | nenhum | World Box2D local exigida em 2D. |
| `maxPlayers` | `16` | Maximo de conexoes aceitas no servidor. |
| `maxPacketBytes` | `65536` | Limite maximo por pacote; minimo 1024. |
| `maxCommandLength` | `256` | Maximo de bytes por linha de comando. |
| `interpolationDelay` | `0.1` | Atraso em segundos para estado remoto. |
| `stateEpsilon` | `0.001` | Diferenca minima para replicar estado server-authoritative. |
| `validationInterval` | `0.25` | Periodo entre solicitacoes de estado de entidade client-authoritative. |
| `playerValidationInterval` | `0.25` | Intervalo minimo entre estados alterados enviados pelo cliente; limite efetivo maximo de 8/s. |
| `maxPlayerSpeed` | `20` | Limite da validacao automatica de velocidade de player em 2D/3D. |
| `positionTolerance` | `0.5` | Margem de deslocamento para validacao automatica. |
| `syncGrace` | `0.25` | Segundos de tolerancia apos correcao. |
| `initialPlayerState` | origem | Estado inicial usado ao criar player. |
| `playerBody` | nenhum | Forma de corpo do player criada no servidor. |
| `createPlayerData` | nenhum | `function(player, profile) -> table|nil` no servidor. |
| `validatePlayerMotion` | nenhum | `function(player, proposedState, mode) -> true|false|state`. |
| `getLocalPlayerState` | nenhum | Callback consultado por frame no cliente; retorna estado local para sincronizacao. |
| `onPositionCorrection` | nenhum | `function(player, correctedState)` no cliente. |
| `validateMotion` | nenhum | Fallback de validacao de entidade client-authoritative. |

Callbacks nao sao transmitidos. Numeros de estado precisam ser finitos.

## Atualizacao e encerramento

```lua
session:update(dt) --> nil
session:close() --> boolean
love.mgold.multiplayer.getActiveMode() --> "2d" | "3d" | nil
```

- `update(dt)` exige numero finito nao negativo e deve rodar uma vez por frame.
  Processa ENet, simula a world associada, solicita/replica estados e aplica
  interpolacao. Sessao fechada gera erro.
- `close()` fecha a sessao, remove entidades/colliders e desconecta peers.
  Retorna `false` se ja estava fechada, senao `true`. O builder tambem destroi
  seu host ENet.
- `getActiveMode()` retorna o modo da unica sessao ativa neste processo, ou
  `nil` sem sessao ativa.

Exemplo:

```lua
local server = love.mgold.server.create({ port = 27015 }):start()

function love.update(dt)
    server:update(dt)
end

function love.quit()
    server:close()
end
```

Somente uma sessao de fisica multiplayer pode estar ativa por processo.
Builders que envolvem host ENet sao tabelas Lua; `host` e `peer` sao userdata
nativos ENet e nao devem ser serializados.

## Eventos por sessao e hooks globais

```lua
session:on(name, callback) --> id numerico
session:off(name, id) --> boolean

love.mgold.multiplayer.hook.add(name, callback) --> id numerico
love.mgold.multiplayer.hook.remove(name, id) --> boolean
love.mgold.multiplayer.hook.run(name, ...) --> true | nil
```

- `name`: nome de evento nao vazio, ate 64 bytes, sem controles.
- `callback`: funcao.
- Hooks de sessao e hooks globais recebem `(session, ...)`.
- `off`/`remove` retornam `false` se nome/ID nao estiver inscrito; caso
  contrario removem e retornam `true`.
- `hook.run` executa os callbacks globais e retorna `true` se algum callback
  retornar exatamente `true`; caso contrario retorna `nil`.
- `session:on` retorna o ID usado por `off`.

Eventos frequentes:

| Evento | Argumentos depois de `session` |
| --- | --- |
| `Connected` | `playerId, localPlayer` |
| `Disconnected` | nenhum |
| `PlayersSnapshot` | `players` |
| `PlayerJoin` / `PlayerLeave` | `player` / `player, reason` |
| `PlayerDataChanged` | `player` |
| `PlayerState` / `PlayerPositionCorrected` | `player, state` |
| `EntityCreated` / `EntityRemoved` | `entity` |
| `EntityDataChanged` | `entity, key, value` |
| `ChatMessageReceived` | `message` |
| `CommandResult` | `success, resultText` |
| `ProtocolViolation` | `peer, reason` (servidor) |
| `RateLimitExceeded` | `player, category` |

Callbacks sao executados sincronamente enquanto a sessao processa eventos ou
atualiza o framework. Evite bloquear o loop do jogo.

## Formato de vetores e copias

Posicao, rotacao e velocidade aceitam `{x=..., y=..., z=...}` ou
`{..., ..., ...}`. Valores devem ser numeros finitos. Metodos `getState`,
`getScoreboard`, `getData` e helpers similares retornam copias de dados, nao
referencias mutaveis a estado de rede. Para modificar estado, use os metodos
de escrita documentados nas paginas de players/entidades.

## Paginas por subsistema

- [Network-Scoreboard.md](./Network-Scoreboard.md): formato de `columns`,
  `rows`, visibilidade, atualizacao de valores e exemplo de UI.
- [Network-Players.md](./Network-Players.md): players, dados, estado e
  validacao/correcao.
- [Network-Entities.md](./Network-Entities.md): autoridade, factories,
  fisica, mapa e dados de entidades.
- [Network-Messaging.md](./Network-Messaging.md): chat, comandos, RPC, input
  e terminal.
- [Network-Gameplay.md](./Network-Gameplay.md): times, modos, voz e vote kick.
