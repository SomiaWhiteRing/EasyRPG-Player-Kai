# 运行模式证据索引

沿用既有 Maniac 250921 的 REA Evidence ID。已有相同文本的 facet 直接引用上一份报告；仅补入缺少的文本。未附原版程序、机器绝对路径或新的版本身份清单。

文本完整保存供复查；实际调查范围是模式、预算及邻接控制流，不表示整份函数所有行为已审核。读取请求见 [read-records.json](read-records.json)。

## 直接引用闭环

- 模式字节：22 个函数；执行预算：13 个函数；合并：33 个函数。
- 伪代码与汇编扫描的函数集合一致；逐项匹配、汇编访问位置和 Evidence ID 见 [mode-reference-inventory.json](mode-reference-inventory.json)。
- 共享模式尾部的 36 条直接指令路由和控制流路径见 [dispatch-mode-routes.json](dispatch-mode-routes.json)；3019 按动态生成指令单列。
- 没有进行原版运行、Player 运行或逐条指令对照。

## 原版文本

|RVA|facet|Evidence ID|文本|
|---|---|---|---|
|00acc0|pseudo_code|ev_170ec7b2364c166ef048268557cb8373e00312b2d0000b5f0d380526c8a9eb78|[text](evidence/0x14000acc0-pseudocode.txt)|
|024760|pseudo_code|ev_11a06318168416fe5a5ea895e1c17e77b993a10d509e52fd3a33cb15598a585c|[text](evidence/0x140024760-pseudocode.txt)|
|024bf0|pseudo_code|ev_9a0b6558fd92cc67665c9fd86c21d151a5a7cdddb2330eecf48addb5a15174ea|[text](evidence/0x140024bf0-pseudocode.txt)|
|024f10|pseudo_code|ev_8f59a7d4993720ba6c9c4a0d061c838bdd578600cbbef476b386d7010f216627|[text](../evidence/0x140024f10-pseudocode.txt)|
|026a90|pseudo_code|ev_e86fa2598fd36005aed21f1b487e21aa39b70123bb1b930298268a8cbaf1a575|[text](evidence/0x140026a90-pseudocode.txt)|
|026d70|pseudo_code|ev_0137bcd789fced44ffff51fa5fc8b0a28f7124ef1cc28ed387a1de7d694574ee|[text](evidence/0x140026d70-pseudocode.txt)|
|0282b0|pseudo_code|ev_80f1db22bdcaa4620217a96444b94c5c8b032d0e40e202f4efaf9e98a49ecbd4|[text](evidence/0x1400282b0-pseudocode.txt)|
|028c00|pseudo_code|ev_020676babb18e8c43be4b06120935cf7422d9830838fadb7c91ac766e5eb689a|[text](evidence/0x140028c00-pseudocode.txt)|
|0293d0|pseudo_code|ev_69e3985cdce91e40b8cc32943d58ad29968132842c7537f729aea3915214cd4e|[text](evidence/0x1400293d0-pseudocode.txt)|
|02d1f0|pseudo_code|ev_c4e16372ca8b89ae2bdb631333320c271438b6202b93386bb164f21c5fc6e014|[text](evidence/0x14002d1f0-pseudocode.txt)|
|030e70|pseudo_code|ev_9985c9f7c5fea0802a6704eb5b1ced63e825c48d72a8cbff291e94f82e7434c8|[text](evidence/0x140030e70-pseudocode.txt)|
|0378d0|pseudo_code|ev_31364e06c09d2e0ca7c00c4ea96432efa989d43b88df70380ecfa190ad42a3a0|[text](evidence/0x1400378d0-pseudocode.txt)|
|037de0|pseudo_code|ev_f3591bbe74c82a120a8e3e929c438690d20b75405c617b40903e6654cf9f52de|[text](evidence/0x140037de0-pseudocode.txt)|
|03de00|pseudo_code|ev_647c5bf99810844161497db925fc97b0592efce93f421ad9f3998ecf3868186d|[text](evidence/0x14003de00-pseudocode.txt)|
|03f620|pseudo_code|ev_468f8996a5d67f41dd844c499c5da9acc688475b54d3f16065e5919321f8cae9|[text](evidence/0x14003f620-pseudocode.txt)|
|040860|pseudo_code|ev_13d42becfa8b2f27db8788501bfd44b73fc929504a6a8dcdd80b8a2f08902349|[text](evidence/0x140040860-pseudocode.txt)|
|045660|pseudo_code|ev_8449e5f0ea9414f1d58cd1a22a079988f29b112968e6950f697497052f8301d8|[text](../evidence/0x140045660-pseudocode.txt)|
|045660|assembly|ev_065b113ae282e0151ebdafb4210490720c2285f9829f0f4e0822f7c044746fdf|[text](evidence/0x140045660-assembly.txt)|
|04e4c0|pseudo_code|ev_dbc5c44341fc93bf61ce249ad2c093880da845be686923227d34ec14973ddf7d|[text](evidence/0x14004e4c0-pseudocode.txt)|
|04e5e0|pseudo_code|ev_63f8514bcf250bae5704a6b0e8fce5511cbf8260693ddef1f87ce4ba5eea4250|[text](evidence/0x14004e5e0-pseudocode.txt)|
|057500|pseudo_code|ev_4a84cae0f12c489000e1c822777db65cc59786a2ef5b930081964df05e1036b7|[text](../evidence/0x140057500-pseudocode.txt)|
|05c4a0|pseudo_code|ev_0f2d614883968ba568c19a5d932ad1aa0efadcc11c5b108289cf9bc217ccc637|[text](evidence/0x14005c4a0-pseudocode.txt)|
|05db00|pseudo_code|ev_5fc8ffd56e09481016dfca6320f87f9b52d2025cd549a906043cb535509c9cef|[text](evidence/0x14005db00-pseudocode.txt)|
|061b20|pseudo_code|ev_35729b69d4f0266a6b9c645da140d9fee9a17bc04c663ceb270cd970645740bf|[text](evidence/0x140061b20-pseudocode.txt)|
|062220|pseudo_code|ev_abedecc5be3a6a8fad271c9b4c6fdff4158917b12170ed1cb7ada0748cf76e24|[text](evidence/0x140062220-pseudocode.txt)|
|0622f0|pseudo_code|ev_062d5850bf22c439386e595e40ec8b9f192619241331403b2336f3aa9d0e939c|[text](evidence/0x1400622f0-pseudocode.txt)|
|063cb0|pseudo_code|ev_67b2cb61e4fcaeb42adf96063d33cf600a38d302a3ba3607748c47d1e36a77be|[text](evidence/0x140063cb0-pseudocode.txt)|
|06ae10|pseudo_code|ev_970bf9d1ac6a2631c3f35984776c715b42bcc83dcfb7e1ade3d9f760cd7e5355|[text](evidence/0x14006ae10-pseudocode.txt)|
|088940|pseudo_code|ev_46b6dba399aef5439e61cb006a27f60e99ca924b54164a31e21e6743d661f654|[text](evidence/0x140088940-pseudocode.txt)|
|088ab0|pseudo_code|ev_14524a5e966fb8b3c7664c606ac833cc2ab47a6b1ecb9d95d085aa24a9e47aaf|[text](evidence/0x140088ab0-pseudocode.txt)|
|08a040|pseudo_code|ev_16d4a9541858daf2f1f4c9c8ad1ca07fe2a829bd5f91b67d41f1e13aedc797a1|[text](evidence/0x14008a040-pseudocode.txt)|
|08c9e0|pseudo_code|ev_58933dd5cfd40c065a467b5ba4eb7375d199d45ec337a409068ae9213b3da6f8|[text](evidence/0x14008c9e0-pseudocode.txt)|
|0903d0|pseudo_code|ev_aba4908e04b242101fc107cc72627e13bf7251ec6f0c2f77c80726796223bb61|[text](evidence/0x1400903d0-pseudocode.txt)|
|0d3cd0|pseudo_code|ev_e7e45c263893696bc2286644f34faee6ad47287c5b1c58a3cbb663dfbcdfb079|[text](evidence/0x1400d3cd0-pseudocode.txt)|
|17bbf0|pseudo_code|ev_9e93b192bbbdffd90a0d7a40613475d39d19f4b6d535de610df29679504ed648|[text](evidence/0x14017bbf0-pseudocode.txt)|
|17bfe0|pseudo_code|ev_564bb324633bd60b10865004f75cf9bd933698000a7ab27ec2e21750ab01d485|[text](evidence/0x14017bfe0-pseudocode.txt)|
|17c0b0|pseudo_code|ev_baceb036a46a763fe550740e0376557cfca838e3a36766bdb893d5b022bb2ae6|[text](evidence/0x14017c0b0-pseudocode.txt)|
|182c20|pseudo_code|ev_a739349720750cdb688500097d8a0e52fe3cc8e371a7c92e3aea9933ea0fa2c5|[text](evidence/0x140182c20-pseudocode.txt)|
|183420|pseudo_code|ev_49f6a4690dc5445ad9a869aefb4ce02fa72d2bb35cb4449e29d3d818ee28a3ef|[text](../evidence/0x140183420-pseudocode.txt)|
|188000|pseudo_code|ev_b45ee5d19cf74158fbf00c63a1700a654e5f439d0aa90067e9e0c2cd13303e75|[text](evidence/0x140188000-pseudocode.txt)|
|1880c0|pseudo_code|ev_b6777577549ba99d776d3cc5934530326e01a5d9e0bfe31fefc36200f26b9504|[text](evidence/0x1401880c0-pseudocode.txt)|
|188680|pseudo_code|ev_a661939574d3eac2ce65f2a8706f57b24736e9039d3b95e3fbcac61097c12c1b|[text](evidence/0x140188680-pseudocode.txt)|
|1894c0|pseudo_code|ev_55133e1bd2ec408cfcbabf7b292c2beedd7b708421c49c30d62f30fa1ab47587|[text](evidence/0x1401894c0-pseudocode.txt)|
|192ba0|pseudo_code|ev_4e3d0f1186db7335677bbd6c5ec447669da78737f7dc6389714c29f18df6eb74|[text](evidence/0x140192ba0-pseudocode.txt)|
|193e60|pseudo_code|ev_e415bdfd858650063097bae0e887c84328d4a332cef8656cd78c9ae8e7189a18|[text](evidence/0x140193e60-pseudocode.txt)|
|198770|pseudo_code|ev_0815b3cfe05cc497956a1ebea041e6ac8220399e8cec370fb09611e8f2d0b1ca|[text](evidence/0x140198770-pseudocode.txt)|
|198a30|pseudo_code|ev_cbed8ddcd1ea96b67eb2e24006e63221779203260375a3d4338ce65c352074b3|[text](evidence/0x140198a30-pseudocode.txt)|
|19a8c0|pseudo_code|ev_52f151d86851e849fe6d271cebbcb705ddb52e059df1705bf6422889e04faa14|[text](evidence/0x14019a8c0-pseudocode.txt)|
|19cdb0|pseudo_code|ev_6f6ac00c5995f5730c5969ebee00c391a858d98f6b592e32a290d0baaacf7fe0|[text](evidence/0x14019cdb0-pseudocode.txt)|
|19d6b0|pseudo_code|ev_565f72352beb0b813480cb4493ae003d78734730395115011ffe5fc703c0ea79|[text](evidence/0x14019d6b0-pseudocode.txt)|
|1a1fc0|pseudo_code|ev_5ddfab324aedd371d3ea31ebe71a3d4168b5da2eaacadc84f4ca61d5b6188ce9|[text](evidence/0x1401a1fc0-pseudocode.txt)|
|1a2200|pseudo_code|ev_1af809e0103d89c0da026cad428cd197e96290075c2614809e42d950333c7f18|[text](evidence/0x1401a2200-pseudocode.txt)|
|1bbfb0|pseudo_code|ev_9087f39f69726d8e9e62482548d84ab98445df669d75311c7dd46521e0e28f5b|[text](evidence/0x1401bbfb0-pseudocode.txt)|
