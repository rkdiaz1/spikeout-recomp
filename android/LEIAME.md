# Port Android (experimental)

1. Faça um fork de tastyrepublic/spikeout-recomp e clone-o no PC.
2. `git checkout -b android && git apply spikeout-android.patch && git add -A && git commit -m "Port Android" && git push -u origin android`
3. No fork, abra a aba Actions e habilite os workflows. O push em `android` inicia "Android APK".
   Se não iniciar, faça outro push (`git commit --allow-empty -m build && git push`).
4. Quando terminar, baixe o artefato `spikeout-android-apk`, instale o APK e escolha o seu `spikeofe.zip`.
5. Se falhar, envie o log da etapa com erro.

Requisitos: Android 13+, ARM de 64 bits, Vulkan. Primeira fase: um jogador. O app abre direto no jogo.
Controles por toque ou controle físico (com um controle conectado, os de toque somem; abra o menu com Select + Start). No menu: direcional, OK, Voltar, Salvar e Carregar; na lista de saves, Apagar. O app guarda uma cópia própria da ROM: depois que o jogo abrir, o arquivo escolhido pode ser apagado do celular (desinstalar o app ou limpar os dados dele apaga a cópia). Sem lobby de internet.
No menu, Controls > botões de toque: arraste para mover, − / + ou pinça para o tamanho, barra de transparência, PADRÃO para voltar ao original.
Energia: o jogo pausa quando o app é minimizado ou a tela é bloqueada, pede a tela a 60 Hz e evita esperas ocupadas.
Não coloque ROMs no repositório. Licença: GPL-3.0.
