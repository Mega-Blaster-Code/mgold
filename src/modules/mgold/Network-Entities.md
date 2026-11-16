# Referencia: entidades e colisores de mapa

Entidade de rede e um objeto de jogo replicado pelo multiplayer. O framework
mantem ID, estado, autoridade, dados de rede e opcionalmente corpo fisico;
nao cria automaticamente o modelo ou a logica visual do jogo.

## Criacao e autoridade

### `server:createEntity(kind, config)`

```lua
local entity = server:createEntity(kind, config) --> Entity
```

- Disponivel somente no servidor.
- `kind`: nome nao vazio do tipo; deve ter factory registrada localmente em
  cada processo que precisa instanciar visual/fisica.
- `config`: tabela de configuracao:

| Campo | Tipo/valor | Uso |
| --- | --- | --- |
| `simulation` | `"server"`, `"client"` ou `"static"`; padrao `"server"` | Quem avanca/simula o objeto. |
| `physics` | `false`, `true` ou tabela; padrao `false` | Corpo fisico opcional. `true` equivale a tabela vazia. |
| `state` | estado completo | Estado inicial; usa vetores position/rotation/velocity. |
| `position` | vetor | Posicao inicial se `state` nao for fornecido. |
| `rotation` | vetor | Rotacao inicial se `state` nao for fornecido. |
| `velocity` | vetor | Velocidade inicial se `state` nao for fornecido. |
| `data` | tabela serializavel | Dados iniciais entregues na criacao da entidade. |
| `networkData` | tabela serializavel | Dados networked iniciais, sujeitos a visibilidade. |
| `networkVisibility` | tabela de booleanos | Marca chaves networked publicas; caso contrario, ficam visiveis ao owner. |
| `owner` | peer ENet | Peer dono de entidade `simulation = "client"`. Use `player.peer` no servidor. |

O servidor atribui um ID inteiro crescente e replica a criacao. Configuracao
invalida gera erro. Entidades `static` nao podem ter fisica (`physics` deve
ser falso/ausente); elas continuam sendo entidades de rede, distintas dos
colisores estaticos de mapa.

Autoridade:

- `"server"`: servidor simula e replica estado quando ele muda; clientes
  recebem/interpolam.
- `"client"`: cliente dono simula e responde a solicitacoes periodicas; o
  servidor valida o estado recebido e pode corrigir.
- `"static"`: sem simulacao fisica e sem sincronizacao de estado por frame;
  alteracoes de estado podem ser explicitamente aplicadas.

Exemplo:

```lua
local grenade = server:createEntity("grenade", {
    simulation = "server",
    physics = { shape = "sphere", radius = 0.18 },
    position = { x = 0, y = 2, z = 0 },
    data = { fuse = 3 },
})

local player = server:getPlayer(playerId)
local car = server:createEntity("car", {
    simulation = "client",
    owner = player.peer,
    physics = { shape = "capsule", radius = 0.6 },
    position = { 0, 0, 0 },
})

local decoration = server:createEntity("lamp", {
    simulation = "static",
    physics = false,
    position = { 2, 0, 1 },
})
```

### Metodos de `Entity`

```lua
entity:getState() --> copia de estado
entity:setState(state) --> entity
entity:setPosition(x, y, z?) --> entity
entity:destroy() --> boolean
entity:getNetworked(key) --> copia do valor | nil
entity:setNetworked(key, value, options?) --> entity
server:removeEntity(id) --> boolean
```

- `getState` le a instancia/body se a factory oferecer; retorna copia.
- `setState` valida os vetores e atualiza body/model/factory. No servidor,
  envia correcao confiavel e inicia `syncGrace`; no cliente, muda somente a
  copia local.
- `setPosition` preserva rotacao e velocidade; `z` ausente vale `0`.
- `destroy` remove da sessao local e chama o callback de destruicao. No
  servidor, a remocao e replicada. Retorna `false` se o ID ja nao existir,
  senao `true`.
- `server:removeEntity(id)` e a forma explicita de remover pelo ID. Retorna
  `false` se nao existir, senao `true`. No servidor remove e replica; chamado
  em cliente remove somente a copia local e nao notifica o servidor, portanto
  nao use para solicitar uma remocao autoritativa.
- `getNetworked` retorna o valor copiado ou `nil`; nao retorna `entity.data`.
- `setNetworked` e somente servidor. `key` e string nao vazia ate 128 bytes;
  `options.public == true` envia a todos, caso contrario somente ao owner.
  Valor precisa ser serializavel.

`config.data` e a carga inicial que as factories recebem; nao e uma API
reativa. Para alteracoes posteriores use `setNetworked`.

## Factories de tipo

### `session:registerType(name, factory)`

```lua
session:registerType(name, factory) --> session
```

Registra uma funcao local para instanciar o tipo. Registre antes do
handshake/criacao da entidade em cada processo que precisa de instancia
visual/fisica. A factory recebe:

```lua
factory(dataCopy, stateCopy, context) --> instanceTable | nil
```

`context` contem:

| Campo | Valor |
| --- | --- |
| `role` | `"server"` ou `"client"` no processo atual. |
| `mode` | `"2d"` ou `"3d"`. |
| `world` | world Box2D em 2D; pode ser `nil` em 3D. |
| `physics` | configuracao de fisica copiada. |
| `entity` | objeto `Entity` gerenciado pela sessao. |
| `localOwner` | `true` se este cliente e o owner local. |

Retorne `nil` para deixar a entidade sem objeto customizado, ou tabela com
campos/metodos:

```lua
return {
    body = body,             -- corpo fisico opcional
    model = model,           -- modelo opcional
    getState = function(self, entity) return state end,
    setState = function(self, state, entity) end,
    destroy = function(self, entity) end,
    validateMotion = function(self, proposed, peer, mode, entity)
        return true -- ou false, ou estado corrigido
    end,
}
```

Cada metodo da instancia recebe `self` primeiro. `getState` deve retornar um
estado valido; `setState` aplica estado na instancia; `destroy` libera recursos
proprios; `validateMotion` e chamado no servidor para movimento de entidade
controlada por cliente. Retorno `true` aceita estado proposto, tabela aceita
estado corrigido, demais valores rejeitam. Uma factory que retorne valor nao
tabela e nao `nil` gera erro.

## Configuracao do corpo fisico

`physics = true` cria a forma padrao. Uma tabela admite:

### Modo 3D (`love.mgold.physics`)

- `shape`: `"sphere"` (padrao) ou `"capsule"`.
- `radius`: raio (padrao `0.2`).

A implementacao atual usa os corpos 3D existentes e consulta colisoes com
modelos estaticos; nao resolve colisao entre corpos dinamicos entre si.

### Modo 2D (`love.physics`)

- `shape`: `"circle"` (padrao), `"rectangle"`/`"box"` ou `"polygon"`.
- `radius`: raio do circulo (padrao `0.5`).
- `width`, `height`: dimensoes do retangulo (padrao `1` cada).
- `points`: array de vertices para poligono; passado a
  `love.physics.newPolygonShape`.
- `density`: densidade do fixture (padrao `1`).

O modo 2D exige a world fornecida em `options.world`. A sessao atualiza essa
world em `session:update(dt)`; nao chame `world:update` uma segunda vez no
mesmo frame por causa do multiplayer.

## Colisores estaticos do mapa

Os colisores do mapa sao fisica estatica compartilhada, nao entidades. O
servidor os registra; o cliente recria o collider local a partir da definicao
recebida.

```lua
server:addStaticCollider(id, definition) --> id
server:removeStaticCollider(id) --> boolean
```

- `id`: string nao vazia ate 128 bytes ou inteiro positivo; IDs duplicados
  geram erro.
- `definition`: tabela de geometria/transformacao; precisa ser serializavel.
- `removeStaticCollider` retorna `false` se o ID nao existir; do contrario
  remove no servidor e replica a remocao.

Para modo 2D, a definicao aceita:

```lua
{ shape = "circle", radius = 1, position = { x = 0, y = 0 } }
{ shape = "rectangle", width = 4, height = 2, position = { x = 0, y = 0 } }
{ shape = "polygon", points = { x1, y1, x2, y2, x3, y3, ... },
  position = { x = 0, y = 0 } }
```

Para modo 3D, `vertices` e um array plano de coordenadas de triangulos: cada
triangulo ocupa nove numeros (`x1,y1,z1,...,x3,y3,z3`). A validacao atual exige
pelo menos nove valores e tamanho multiplo de tres; para malha triangular
completa, forneca um numero de valores multiplo de nove. Transformacoes:

```lua
{
    vertices = { ... },
    position = { x = 0, y = 0, z = 0 }, -- padrao origem
    rotation = { x = 0, y = 0, z = 0 }, -- padrao zero
    scale = { x = 1, y = 1, z = 1 },    -- padrao unitario
    zoneSize = 0,                       -- tamanho de zona da fisica mgold
}
```

Adicione colisores antes da conexao para que entrem no snapshot inicial, ou
durante o jogo para enviar uma alteracao incremental. O servidor mantem uma
revisao do mapa; nao ha replicacao de collider por frame.

## Eventos

`EntityCreated(session, entity)` e `EntityRemoved(session, entity)` notificam
criacao/remocao local. `EntityDataChanged(session, entity, key, value)` e
emitido no cliente ao receber mudanca networked; `value == nil` indica remocao
da chave publica local. Consulte [Network-API.md](./Network-API.md) para
registro e remocao de callbacks.
