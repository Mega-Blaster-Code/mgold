# Referencia: scoreboard multiplayer

O framework guarda e replica linhas publicas. O cliente desenha automaticamente
uma tabela ao segurar Tab, depois do `love.draw` do jogo. A UI nativa usa
`love.graphics` e preserva o callback existente.

## Formato retornado

```lua
local rows = session:getScoreboard()
-- rows = {
--     { { "name", "Lia" }, { "color", { 1, 0.4, 0.2, 1 } }, { "score", 123 } },
--     { { "name", "Noah" }, { "color", { 1, 1, 1, 1 } } },
-- }
```

O retorno e um array de linhas. Cada linha e um array ordenado de pares
`{ chave, valor }`:

- `pair[1]` e o nome string do campo;
- `pair[2]` e o valor serializavel desse campo;
- pares vao em ordem `name`, `color` por padrao, seguidos por outras chaves
  publicas em ordem alfabetica;
- jogadores/linhas vao em ordem crescente de ID do servidor.

Nao e um mapa Lua: use `ipairs(row)` e acesse `pair[1]`/`pair[2]`. A ordem e
deterministica em chamadas sucessivas para o mesmo conjunto de chaves
publicas. Campos removidos deixam de aparecer; campos novos sao inseridos em
ordem alfabetica apos as chaves prioritarias.

`name` e `color` sao as unicas chaves iniciais. Nome vem do perfil validado
ou `"Player<ID>"`. Cor inicial e `{ 1, 1, 1, 1 }` (RGBA normalizado). `score`,
`health`, `team`, `ping` e outras estatisticas nao sao adicionados ao
scoreboard automaticamente. O jogo escolhe quais campos extras criar.

## Campos publicos e privados

Somente campos publicos aparecem nas linhas e sao serializados para clientes.
Campos privados nao sao enviados **nem para o proprio dono da linha**.
`session:getScoreboard()` tambem filtra para listar somente campos publicos.

```lua
player:setData(key, value, { public = true })  -- campo do scoreboard
player:setData("inventory", items)             -- privado por padrao
```

- `key`: string nao vazia de ate 128 bytes.
- `value`: `nil`, boolean, string, numero finito ou tabela serializavel sem
  ciclos.
- `options.public`: precisa ser exatamente `true` para aparecer nos clientes.
- `setData` e uma operacao do servidor; retorna o Player e replica a mudanca.
- Os campos iniciais `name` e `color` sao publicos. Se alterar `color` com
  `setData`, passe `{ public = true }` para manter a cor visivel.

Dados retornados por `createPlayerData` sao privados por padrao. Para publicar
uma estatistica, inicialize-a com `setData` publico:

```lua
server:on("PlayerJoin", function(_, player)
    player:setData("kills", 0, { public = true })
    player:setData("score", 0, { public = true })
    player:setData("color", { 0.25, 0.7, 1, 1 }, { public = true })
end)

local function awardKill(killer)
    killer:setData("kills", (tonumber(killer:getData("kills")) or 0) + 1,
        { public = true })
    killer:setData("score", (tonumber(killer:getData("score")) or 0) + 100,
        { public = true })
end
```

O servidor filtra dados no snapshot inicial, no evento de entrada e em cada
atualizacao. O cliente nunca recebe a tabela privada original. Nao coloque
segredos, inventario privado ou permissoes em campos publicos.

## Prioridade de ordenacao opcional

```lua
session:setScoreboardColumns({ "name", "color", "kills", "score" }) --> session
```

O array especifica somente a ordem prioritaria dentro de cada linha; nao e
uma whitelist. Todos os campos publicos ainda aparecem, e os nao listados
seguem em ordem alfabetica. Chaves duplicadas sao ignoradas. Por padrao, a
prioridade e `{ "name", "color" }`.

Cada processo tem sua propria configuracao local. Configure no cliente se
precisar de ordem customizada; a lista nao e enviada pela rede.

## Acessar valores

```lua
local rows = client:getScoreboard()
for _, row in ipairs(rows) do
    for _, pair in ipairs(row) do
        local key, value = pair[1], pair[2]
        print(key, value)
    end
end
```

O retorno e uma copia; altera-lo nao muda estado do Player. Para converter
temporariamente uma linha em mapa:

```lua
local function rowToMap(row)
    local result = {}
    for _, pair in ipairs(row) do result[pair[1]] = pair[2] end
    return result
end

local row = rowToMap(client:getScoreboard()[1])
print(row.name, row.score or "-")
```

## UI padrao da engine

```lua
local client = love.mgold.client.create({
    scoreboardEnabled = true, -- opcional; true por padrao
})
client:connect(address, port)

client:setScoreboardColumns({ "name", "color", "kills", "score" })
client:setScoreboardEnabled(false) -- desativa somente a UI automatica
```

Depois que o cliente chama `client:update(dt)`, a engine integra seu desenho ao
callback `love.draw` atual. O jogo nao precisa implementar um callback proprio
para exibir a tabela. Por padrao ela aparece somente enquanto
`love.keyboard.isDown("tab")` retornar `true`. A UI:

- mostra todos os jogadores e todas as chaves publicas;
- usa `name` como texto branco;
- usa `color` (array `{r,g,b,a}` ou tabela `{r=...,g=...,b=...,a=...}`) para
  colorir os outros valores da linha;
- nao mostra uma coluna separada para `color`;
- calcula as larguras a partir do texto atual e mantem a ordem das chaves;
- e habilitada por padrao; `scoreboardEnabled = false` na criacao ou
  `client:setScoreboardEnabled(false)` desativa o desenho automatico;
- `client:isScoreboardEnabled()` informa o estado atual;
- `client:drawScoreboard()` desenha manualmente e retorna `true` se desenhou
  ou `false` se Tab nao estiver pressionado.

O wrapper chama o `love.draw` do jogo primeiro e a UI depois. Se o jogo definir
`love.draw` novamente durante a execucao, a engine o incorpora no proximo
`client:update(dt)`. Se o jogo tambem desenhar seu proprio scoreboard, desative
a UI automatica para evitar duplicacao.

`client:setScoreboardEnabled(enabled)` exige boolean e retorna o cliente.
`scoreboardEnabled` tambem pode ser definido como boolean nas opcoes passadas
a `client.create`. A opcao controla apenas a interface automatica; nao altera
`getScoreboard()` nem os dados replicados.

Para desenhar manualmente a tabela padrao, desative o desenho automatico e
chame `client:drawScoreboard()` em `love.draw`.

## UI customizada

`love.mgold.multiplayer.scoreboard.create(columns?)` cria um helper independente
da UI automatica. Use `board:draw(client)` dentro de `love.draw` quando quiser
renderizar uma segunda apresentacao. O helper nao registra callbacks. Se usar
uma UI propria, chame `client:setScoreboardEnabled(false)` para evitar mostrar
tambem a tabela padrao.

`love.mgold.multiplayer.scoreboard.create(columns?)` aceita uma lista opcional
de chaves prioritarias para a UI. Sem argumento usa `{"name", "color"}`.
`board:setColumns(columns)` altera essa prioridade local e retorna `board`;
`board:getRows(session)` retorna o mesmo formato que `session:getScoreboard()`.
`columns` deve ser um array de nomes validos (strings nao vazias de ate 64
bytes); chaves repetidas sao ignoradas. O helper nao envia configuracao pela
rede.

## Ordenar jogadores por uma estatistica

As linhas sao retornadas por ID, nao por score. Para outro ranking, converta
as linhas e ordene uma copia:

```lua
local function rowToMap(row)
    local result = {}
    for _, pair in ipairs(row) do result[pair[1]] = pair[2] end
    return result
end

local ranking = {}
for _, row in ipairs(client:getScoreboard()) do
    ranking[#ranking + 1] = rowToMap(row)
end
table.sort(ranking, function(a, b)
    return (tonumber(a.score) or 0) > (tonumber(b.score) or 0)
end)
```
