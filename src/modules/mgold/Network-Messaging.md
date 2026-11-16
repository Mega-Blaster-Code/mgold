# Referencia: chat, comandos, RPC e input

Esta pagina detalha as mensagens da aplicacao. Receber um pedido do cliente
nao torna o conteudo confiavel: valide permissao, formato, limites e estado do
jogo no servidor.

## Chat

### Opcoes de sessao `chat`

```lua
chat = {
    enabled = true,   -- padrao
    global = true,    -- padrao
    team = true,      -- padrao
    private = true,   -- padrao
    maxLength = 256,  -- padrao
    filter = function(text, player, channel, target) return text end,
}
```

- `enabled`: liga/desliga o chat inteiro.
- `global`, `team`, `private`: habilitam cada canal.
- `maxLength`: comprimento maximo da mensagem em bytes; precisa ser positivo
  e nao maior que `maxPacketBytes`.
- `filter`: opcional, somente servidor. Recebe texto original, objeto Player
  remetente, nome do canal e Player alvo (somente private; nos outros casos
  pode ser `nil`). Deve retornar string nao vazia dentro do limite; qualquer
  outro retorno rejeita a mensagem.

Chamadas de cliente:

```lua
client:chat(text) --> boolean       -- global
client:teamChat(text) --> boolean   -- membros do mesmo time
client:privateChat(targetId, text) --> boolean
```

Argumentos sao strings nao vazias dentro de `maxLength`; `targetId` deve ser
ID valido. Retorno `false` significa argumento invalido (ou cliente sem
destino valido); `true` significa pacote enfileirado, nao garante entrega.
Todas as chamadas exigem role client; chamar em servidor gera erro.

O servidor entrega mensagens privadas ao remetente e destinatario; mensagem
de time aos jogadores no mesmo time; global a todos. O cliente recebe:

```lua
session:on("ChatMessageReceived", function(session, message)
    -- message.channel: "global", "team", "private" ou "system"
    -- message.senderId, message.senderName, message.targetId
    -- message.text, message.time
end)
```

`message` e uma tabela copiada do pacote. Campos opcionais podem faltar em
mensagem de sistema.

### Mensagem de sistema do servidor

```lua
server:sendMessage(text, targetId?) --> boolean
```

`text` e string ate `chat.maxLength`. Sem alvo, envia a todos; com `targetId`,
envia apenas ao jogador. Retorna `false` se o ID alvo nao existir; retorna
`true` quando mensagem foi enfileirada. Role diferente de server gera erro.

## Comandos

### Registrar comando (servidor)

```lua
server:registerCommand(name, callback, options?) --> server
```

- `name`: 1-48 bytes, caracteres permitidos: letras, numeros, `_` e `-`.
  O registro e convertido para minusculas.
- `callback(player, args, server)`: `player` e Player remetente, ou `nil`
  quando executado localmente; `args` e array de strings sem o nome do
  comando; `server` e sessao.
- `options.admin = true`: exige `player.data.admin == true` ou
  `player.data.rank == "admin"` para chamada remota.
- `options.serverOnly = true`: rejeita execucao remota.
- `options.permission(player, server)`: permissao customizada; precisa
  retornar valor truthy para permitir.

Argumentos podem ser agrupados por aspas duplas; barra invertida dentro de
aspas escapa o proximo caractere. Linha malformada retorna erro de sintaxe.
Comando desconhecido retorna `"unknown command"`.

### Executar comando

```lua
client:runCommand(line) --> true | false, reason?
server:runCommand(line) --> success, result
```

- Cliente: envia requisicao confiavel se `peer` existe e `ready == true`;
  `true` significa que a requisicao foi enviada, nao que o comando foi
  autorizado/executado. Desconectado retorna `false, "not connected"`.
- Servidor: executa localmente sem player remoto e retorna booleano de sucesso
  mais texto de resultado.
- Linha tem limite `maxCommandLength` (padrao 256). Comandos remotos estao
  limitados a 5 por janela de 5 segundos por peer.
- Resultado remoto chega como evento
  `CommandResult(session, success, resultText)`. Falha do callback gera
  `CommandError` no servidor e resposta `"command failed"`.

Comandos registrados por padrao no servidor:

| Nome | Comportamento |
| --- | --- |
| `status` | Retorna quantidade de players e ID/nome/ping de cada player. |
| `say <texto>` | Envia mensagem global de sistema. |
| `kick <id>` | Remove player; exige permissao admin quando chamado remotamente. |

## RPC

RPC e uma mensagem identificada por nome com payload serializavel. Nao e
execucao de codigo, e nao tem retorno automatico.

### Registrar

```lua
session:registerRPC(name, callback, options?) --> session
```

- `name`: string nao vazia ate 64 bytes.
- Callback no servidor: `callback(player, payload, session)`.
- Callback no cliente: `callback(payload, session)`.
- No servidor, `options.permission(player, payload, session)` precisa retornar
  exatamente `true`; caso contrario a chamada e recusada.
- Registre o nome no processo que recebe a RPC antes de sua chegada.

### Enviar

```lua
client:sendRPC(name, payload) --> boolean
player:sendRPC(name, payload) --> player
```

- Cliente envia ao servidor; `false` se nao conectado ou nome invalido.
- Servidor envia ao cliente dono do Player; retorna o mesmo Player.
- Payload aceita `nil`, boolean, string, numero finito e tabelas sem ciclos
  com chaves string/number/boolean; profundidade maxima 64.
- RPC de cliente usa entrega confiavel e o servidor limita a 20 por segundo
  por peer.
- `true` indica envio, nao sucesso de callback. RPC server-to-client sem
  handler registrado no cliente nao executa acao.

Exemplo com validacao do servidor:

```lua
server:registerRPC("buyPotion", function(player, payload, session)
    if type(payload) ~= "table" or payload.item ~= "potion" then return end
    local coins = tonumber(player:getData("coins")) or 0
    if coins < 10 then return end
    player:setData("coins", coins - 10)
    player:setData("potions", (tonumber(player:getData("potions")) or 0) + 1)
end, {
    permission = function(player, payload, session)
        return type(payload) == "table" and payload.item == "potion"
    end,
})

client:sendRPC("buyPotion", { item = "potion" })
```

O cliente nao decide o saldo final: o servidor le e altera a economia.

## Input frequente

Use input para mensagens frequentes (por exemplo, direcao/botoes) que nao
precisem da entrega confiavel de uma RPC.

```lua
server:registerInput(name, callback, options?) --> server
client:sendInput(name, payload) --> boolean
```

- `registerInput` somente no servidor; `name` e string nao vazia ate 64 bytes.
- `callback(player, payload, session)` recebe o remetente e payload validado
  para serializacao.
- `options.permission(player, payload, session)` precisa retornar exatamente
  `true` quando definido.
- `sendInput` somente cliente pronto; retorna `false` se nao conectado/nome
  invalido e `true` se pacote foi enfileirado.
- Input usa transporte nao confiavel, limitado a 60 por segundo por peer.
  Pacotes perdidos nao sao reenviados; nao o use como unica fonte de estado
  critico.

## Terminal helper

`love.mgold.terminal.create(client, options?)` cria um buffer, nao uma janela
ou interface:

```lua
local terminal = love.mgold.terminal.create(client, { maxLines = 100 })
terminal:submit(line) --> mesmos retornos de client:runCommand
terminal:write(text) --> nil
terminal:getLines() --> copia do array de strings
terminal:clear() --> nil
```

`maxLines` e inteiro positivo (padrao 100). `submit` remove um prefixo
opcional `>` e espacos iniciais, escreve o prompt e envia o comando. Os
eventos `CommandResult`/`CommandError` sao adicionados ao buffer. O jogo
continua responsavel por teclado e renderizacao.

## Erros e limites de dados

Payloads sao copiados/serializados antes do envio. Ciclos, funcoes, userdata,
threads, chaves de tabela e numeros NaN/infinitos nao sao aceitos; erro de
serializacao e reportado como erro Lua. Limites por taxa e permissoes rejeitam
mensagens no servidor e podem emitir `RateLimitExceeded`, `InvalidRPC`,
`UnauthorizedRPC`, `InvalidInput`, `UnauthorizedInput` ou `ProtocolViolation`.
