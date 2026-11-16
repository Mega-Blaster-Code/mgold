# Referencia: players e estado do jogador

O servidor cria os IDs de player no handshake. Um `Player` e uma tabela Lua
gerenciada pela sessao; no servidor ela representa a conexao e no cliente e
um snapshot dos dados recebidos.

## Encontrar players

```lua
session:getPlayers() --> array de Player
session:getPlayer(id) --> Player | nil
```

- `getPlayers()` retorna um array novo ordenado por ID. Os objetos `Player`
  sao os objetos gerenciados pela sessao, nao copias.
- `getPlayer(id)` procura o ID atribuido pelo servidor. Retorna `nil` se ele
  nao estiver na tabela local.
- `player.id`: ID numerico da sessao; nao e nome de conta persistente.
- `player.data`: tabela de dados local do objeto. No cliente contem somente
  os campos que aquele cliente pode ver.
- `player.team`: nome do time ou `nil`.
- `player.connected`: estado local da conexao/snapshot.
- `player.peer`: peer ENet no servidor; nao deve ser serializado ou guardado
  como identificador de jogo.

### Remover um player (servidor)

```lua
server:removePlayer(playerId, reason?) --> boolean
```

`playerId` e o ID numerico local do player. `reason` e um motivo opcional
(convencionalmente string), incluido no evento de saida; o padrao e
`"kicked"`. Retorna `false` se o ID nao existir; senao desconecta e remove o
player, limpa sua referencia de time e retorna `true`. Chamar em cliente gera
erro. O servidor pode usar uma razao propria, por exemplo `"match ended"`;
isso nao cria um sistema de banimento persistente.

## Dados de player

### `player:getData(key?)`

- Sem `key`, retorna uma copia da tabela de dados visiveis.
- Com uma chave, retorna uma copia do valor daquela chave ou `nil`.
- Alterar a tabela retornada nao modifica o player.

### `player:setData(key, value, options?)`

```lua
player:setData(key, value, { public = true|false }?) --> player
```

- Disponivel somente no servidor; no cliente gera erro.
- `key`: string nao vazia de ate 128 bytes, sem caracteres de controle.
- `value`: tipos serializaveis (ver limites abaixo).
- `options.public`: somente o boolean `true` publica o campo aos demais
  clientes. Ausente ou `false` deixa o campo privado.
- Retorna o mesmo objeto `Player`.
- O dado e transmitido como atualizacao; use este metodo para modificar dados,
  em vez de alterar `player.data` diretamente.

Dados padrao publicos do player: `name` (perfil validado ou `"Player<ID>"`)
e `color` (`{ 1, 1, 1, 1 }`, RGBA). Outros campos, incluindo `health` e
`score`, nao sao criados automaticamente. Dados retornados por
`createPlayerData` sao aplicados com `setData` sem opcoes e, portanto,
privados por padrao.

```lua
local server = love.mgold.server.create({
    createPlayerData = function(player, profile)
        -- profile contem apenas name, se fornecido pelo cliente.
        return { class = "scout", coins = 0 }
    end,
}):start()

server:on("PlayerJoin", function(_, player)
    player:setData("kills", 0, { public = true })
end)

local function awardKill(killer)
    killer:setData("kills", (killer:getData("kills") or 0) + 1, { public = true })
    killer:setData("score", (killer:getData("score") or 0) + 100, { public = true })
end
```

`createPlayerData(player, profile)` e uma opcao de sessao do servidor. Deve
retornar uma tabela ou `nil`. `player` e o objeto criado; `profile` e uma
copia do perfil recebido (atualmente somente `name` e transmitido).

## Estado e correcoes

Estado usa tres vetores finitos:

```lua
{
    position = { x = 1, y = 2, z = 3 },
    rotation = { x = 0, y = 0, z = 0 },
    velocity = { x = 0, y = 0, z = 0 },
}
```

Cada vetor aceita campos nomeados `x/y/z` ou indices `[1]/[2]/[3]`. `position`
e obrigatorio. Rotacao e velocidade ausentes recebem zero. Cada componente
deve ser numero finito.

### Leitura e escrita

```lua
player:getState() --> copia de { position, rotation, velocity }
player:setState(state) --> player                 -- somente servidor
player:setPosition(x, y, z?) --> player           -- somente servidor
player:getPing() --> numero em milissegundos
```

- `getState()` retorna uma copia. Alterar o retorno nao move o jogador.
- `setState(state)` substitui o estado, atualiza o corpo de fisica associado,
  envia uma correcao confiavel e inicia o periodo de tolerancia `syncGrace`.
- `setPosition` preserva rotacao/velocidade atuais; `z` ausente vira `0`.
- `getPing()` consulta o RTT do peer ENet no servidor. Se nao houver peer,
  retorna `0`; nao e uma medicao garantida no snapshot do cliente.

### Simulacao local do cliente

```lua
client:setLocalPlayerState(state) --> client
```

Guarda o estado local atual. A sessao envia o estado quando ele muda (com
limite configuravel por `playerValidationInterval`) para o servidor validar e
replicar aos outros clientes. Atualize-o regularmente no loop do jogo.
Alternativamente, defina no cliente:

```lua
getLocalPlayerState = function()
    return playerController:getNetworkState()
end
```

O callback de opcao `getLocalPlayerState()` nao recebe argumentos e e
consultado durante `session:update(dt)`. O retorno deve ter o formato de
estado valido acima; `nil` pausa atualizacoes e um estado invalido gera erro.
O estado sincronizado inclui posicao, rotacao (angulo em `rotation.z` no modo
2D) e velocidade. Cada cliente mantem snapshots dos jogadores remotos e
interpola os tres vetores usando pelo menos `interpolationDelay` (padrao
`0.1` s); quando o intervalo observado entre snapshots for maior, usa esse
intervalo como atraso para manter a interpolacao continua.
`player:getState()` retorna o estado interpolado mais recente; correcao
autoritativa e aplicada imediatamente. A sessao aplica o estado interpolado
tambem a um corpo `playerBody`, se configurado. O jogo deve aplicar
`player:getState()` ao modelo/avatar visual que registrou.

Exemplo sem callback:

```lua
function love.update(dt)
    client:setLocalPlayerState({
        position = { x = avatar.x, y = avatar.y, z = avatar.z },
        rotation = { x = avatar.pitch, y = avatar.yaw, z = avatar.roll },
        velocity = { x = avatar.vx, y = avatar.vy, z = avatar.vz },
    })
    client:update(dt)
end
```

O cliente envia somente quando o estado diverge do ultimo enviado (epsilon
padrao `0.001`), no maximo 8 vezes por segundo. O envio cliente-servidor e a
replicacao server-clients sao confiaveis. O servidor emite `PlayerState` e
propaga aos demais clientes apos aceitar o estado; chamadas `setState` do
servidor sao correcao autoritativa.

### Validacao do servidor

```lua
validatePlayerMotion = function(player, proposedState, mode)
    return true                 -- aceita proposedState sem alteracao
    -- return false            -- rejeita e corrige com o estado do servidor
    -- return correctedState   -- aceita e usa o estado corrigido
end
```

`proposedState` e uma copia do estado informado pelo cliente. `mode` e `"2d"`
ou `"3d"`. A tabela retornada como correcao tambem precisa conter vetores
validos. Somente `true` aceita o estado original; `false`, `nil` ou qualquer
outro retorno o rejeita. Ao retornar um estado corrigido, o servidor envia a
correcao tambem ao proprio cliente, inicia `syncGrace` e emite
`PlayerPositionCorrected`; os demais clientes recebem o estado autoritativo.

Sem callback, os modos 2D e 3D limitam velocidade e deslocamento. No modo 3D,
se houver corpo fisico, a validacao tambem consulta colisao estatica; no modo
2D nao e feita consulta automatica de colisao. Isso e verificacao basica, nao
substitui regras especificas de movimento.

Uma correcao recebida pelo player local chama:

```lua
onPositionCorrection = function(player, correctedState)
    -- Ajuste tambem o controlador/personagem visual local.
end
```

O callback e executado no cliente quando chega uma mensagem de correcao.
Atualizacoes normais de estado nao significam necessariamente correcao.

## Times

`player:setTeam(name)` e um atalho para
`session:setPlayerTeam(player.id, name)`. E uma operacao do servidor; chamar
no cliente gera erro, pois nao existe um pedido cliente-servidor para troca
de time. No servidor:

```lua
server:setPlayerTeam(playerId, teamName?) --> boolean
```

`teamName == nil` remove o jogador do time. Retorna `false` se o player nao
existir, o nome do time nao existir ou o time estiver cheio. Consulte
[Network-Gameplay.md](./Network-Gameplay.md) para criar times.

## RPC para um jogador

```lua
player:sendRPC(name, payload) --> player
```

Somente no servidor. Envia ao cliente dono deste player uma RPC confiavel.
`name` e string nao vazia ate 64 bytes. `payload` deve obedecer os limites de
serializacao. O retorno e o mesmo player; isto nao garante que o cliente tenha
executado um callback com aquele nome.

## Limites de serializacao

Dados enviados pela rede podem conter `nil`, booleanos, strings, numeros
finitos e tabelas sem ciclos com chaves string, number ou boolean. Funcoes,
userdata, threads, ciclos, chaves-tabela e numeros `NaN`/infinitos nao sao
suportados. A profundidade maxima e 64.

## Eventos relacionados

Callbacks registrados em `session:on` recebem a sessao primeiro:

| Evento | Argumentos depois da sessao | Quando ocorre |
| --- | --- | --- |
| `Connected` | `playerId, localPlayer` | Cliente recebeu o snapshot inicial. |
| `PlayersSnapshot` | `players` | Cliente aplicou a lista inicial. |
| `PlayerJoin` | `player` | Player entrou/foi adicionado ao snapshot. |
| `PlayerLeave` | `player, reason` | Player saiu. |
| `PlayerDataChanged` | `player` | Chegou um snapshot de dados do player. |
| `PlayerState` | `player, state` | Estado aceito/recebido sem marcacao de correcao. |
| `PlayerPositionCorrected` | `player, state` | Estado corrigido pelo servidor. |
| `PlayerTeamChanged` | `player, teamName` | Evento local do servidor quando o time e alterado. O cliente recebe `PlayerDataChanged` com o snapshot atualizado. |

Uma forma comum de atualizar a UI e invalidar o scoreboard nos eventos
`PlayerDataChanged`, `PlayerJoin`, `PlayerLeave` e `PlayerTeamChanged`.
Consulte [Network-Scoreboard.md](./Network-Scoreboard.md) para formato de
linhas, colunas e exemplo de renderizacao.
