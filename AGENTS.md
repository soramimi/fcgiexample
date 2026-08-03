# AGENTS.md

このファイルは、本リポジトリを扱う AI コーディングエージェント向けの情報です。

## プロジェクト概要

`fcgiexample` は、C++ で書かれた最小限の **FastCGI サーバー／アプリケーションのサンプル** です。

- `serv`（`fcgiserv`）: 簡易 HTTP サーバー。リクエストを受けて FastCGI アプリを起動・橋渡しする。**`/static/` 以下は `static/` ディレクトリから静的ファイルを配信する。**
- `app`（`fcgiapp`）: FastCGI 対応アプリケーション。サーバーからのリクエストに応答する。
- `fcgi/`: FastCGI リファレンス実装（libfcgi）を内包。

## ディレクトリ構成

```
.
├── AGENTS.md           本ファイル
├── fcgiapp.pro         app 側の Qt/qmake プロジェクトファイル
├── fcgiserv.pro        serv 側の Qt/qmake プロジェクトファイル
├── fcgi/               FastCGI ライブラリ（libfcgi + include）
├── serv/               HTTP サーバー＋FastCGI ブリッジのソース
├── app/                FastCGI アプリケーションのソース
├── static/             静的ファイル配信ディレクトリ
├── test/               Bash ベースの結合テスト群
├── _bin/               ビルド済み実行ファイルの出力先
├── build/              qmake 中間生成物
└── misc/               その他雑ファイル
```

## ビルド

Qt（qmake）を使ってビルドします。コンソールアプリケーションで、Qt ライブラリには依存しません。

```bash
# serv のビルド
qmake fcgiserv.pro
make

# app のビルド
qmake fcgiapp.pro
make
```

または Qt Creator で `fcgiapp.pro` / `fcgiserv.pro` を開いてビルド可能です。

### 注意

- `DESTDIR` は両方とも `$$PWD/_bin` に設定されている。
- Linux では `fcgiserv` のリンクに `-lpthread` が必要。
- Windows 用コードも条件コンパイルで含まれているが、現在の主なターゲットは Linux/Unix。

## 実行

```bash
# 1. サーバーを起動（ポート 5000）
./_bin/fcgiserv

# 2. 別ターミナルでアプリを起動（CGI stdin モード、サーバーが起動する）
./_bin/fcgiapp

# または TCP 待受モードで起動
./_bin/fcgiapp -p 3000

# ブラウザや curl でアクセス
curl http://localhost:5000/app/
curl http://localhost:5000/hello/
curl http://localhost:5000/static/index.html
```

補足:

- `fcgiserv` の `main()` は起動時に `/home/soramimi/develop/fcgiexample/_bin` へ `chdir()` する前提になっている。そのため、FastCGI アプリ実行パスや相対パス解決は `_bin` 基準で考えること。

## テスト

基本的な結合テストは `test/` 配下にある。

```bash
# 全テスト実行
./test/all.sh

# 個別実行
./test/basic.sh
./test/error_cases.sh
./test/websocket.sh

# デバッグトレース付き
DEBUG=1 ./test/all.sh
```

- `test/common.sh` にビルド、サーバー起動、後片付け、アサーションの共通処理を集約している。
- `test/basic.sh` のみ `fcgiapp` も必要とする。`fcgiapp.pro` が更新されていない限り、テスト用 Makefile は再利用される。
- `test/error_cases.sh` は `curl` に加えて `/dev/tcp` を使い、生の不正 HTTP リクエストも検証する。
- `test/websocket.sh` は Python 3 標準ライブラリのみで WebSocket ハンドシェイクとエコーを確認する。

## 主要コンポーネント

### serv 側

| ファイル | 役割 |
|---|---|
| `main.cpp` | HTTP ハンドラ（`MyHandler`）とサーバー起動。`/app/` で FastCGI を呼び出し、`/static/` で静的ファイルを配信する。 |
| `httpserver.cpp/h` | 簡易 HTTP/1.1 サーバー。リクエストパース、レスポンス送信、WebSocket ハンドシェイク。 |
| `FcgiProcess.cpp/h` | FastCGI プロセス／ソケット接続の管理。Unix ドメインソケット、INET ソケット、子プロセス起動に対応。 |
| `socket.h` | クロスプラットフォームなソケット型とヘルパー。 |
| `thread.cpp/h`, `event.cpp/h`, `mutex.h` | スレッド・イベント・ミューテックスのユーティリティ。 |
| `misc.cpp/h`, `strformat.h`, `joinpath.cpp/h` | 文字列処理、パス結合など。 |
| `base64.cpp/h`, `sha1.c/h` | WebSocket ハンドシェイク用の Base64/SHA1。 |
| `httpstatus.cpp/h`, `httpstatus.txt` | HTTP ステータスコード定義。 |
| `debug.cpp/h` | デバッグログ出力。 |

### app 側

| ファイル | 役割 |
|---|---|
| `main.cpp` | `FCGI_Accept()` ループ。時刻と環境変数を返す。 |
| `myfcgi.c/h` | TCP または Unix ドメインソケットで待ち受けるためのヘルパー。 |
| `fcgiapp.c` | FastCGI 標準入出力のラッパー実装。 |

## コードスタイル・注意事項

- C++17 を使用（`CONFIG += c++17`）。`std::string_view` / `std::optional` 等を使用するため。
- タブインデント（タブ幅 4 文字）が基本。
- プラットフォーム分岐は `#ifdef _WIN32` / `#else` で行う。
- `serv/main.cpp` 内の `invoke_fastcgi()` は、FastCGI プロトコルのエンコード／デコードを直接実装している。プロトコル変更時はここを確認すること。
- FastCGI バックエンドの起動方式は `cmd` 文字列で切替可能（`serv/main.cpp` 内の `invoke_fastcgi()` 冒頭で定義）。現在の既定は `unix:/tmp/foo.sock` であり、プロセス直接起動の `./fcgiapp` はコメントで残されている。:
    - `./fcgiapp`（プロセス直接起動）: リクエストごとに `fork()` + `execvp()` で起動。リスニングソケットは1回の `accept()` で消費されるため、毎リクエスト再起動が必要（`proc_expired` フラグで管理）。
    - `unix:/path/to.sock`: Unix ドメインソケット接続。接続済みソケットはリクエスト間で再利用される。
    - `inet:host:port`: TCP 接続。同上。
- デフォルトのバインドアドレスは `127.0.0.1`（ループバックのみ）。外部公開する場合は `HTTP_Server::setBindAddress("0.0.0.0")` を呼び出すこと。
- **静的ファイル配信**: 既存のハンドラパターン（`/app`、`/sock`、`/hello` 等）にマッチしない **GET** リクエストをフォールバックとして、`static/` ディレクトリから配信する。`misc::normalize_path()` を使って `..` 等を解決し、ルート外へのパストラバーサルを防ぐ。配信対象ファイルは 8MiB まで。`POST` 等の他メソッドは `405`、ディレクトリそのものや存在しないファイルは `404`、サイズ超過は `413` を返す。

### 既知の堅牢化対応

以下は `serv` 側で既に実施済みの安定性・セキュリティ強化です。新規に変更する際はこれらを損なわないようにしてください。

- **HTTP リクエスト制限**: 1 行あたり 8KiB、ヘッダー全体 64KiB、ボディ 8MiB の上限を設けている。超過時は接続を切断または `413` を返す。
- **ソケットタイムアウト**: `accept()` 後の接続ソケットと FastCGI 上流ソケットに `SO_RCVTIMEO`/`SO_SNDTIMEO`（30秒）を設定し、Slowloris 攻撃や無応答クライアント/上流を防ぐ。`accept()` は `EINTR` を再試行する。
- **URL 正規化・パストラバーサル防止**: `misc::normalize_path()` で `.` / `..` を解決し、不正なパスは `400` を返す。
- **Content-Length 検証**: 重複する `Content-Length` ヘッダーを拒否して `400` を返す（HTTP request smuggling 対策）。不正な値は `413` を返す。
- **HTTP バージョン厳格化**: keep-alive は `HTTP/1.1` 以上（`maj == 1 && min >= 1`）のみ有効。
- **POST ボディの読み捨て**: keep-alive 接続でハンドラがボディを消費しなかった場合、`Content-Length` 分を自動的に読み捨てる。
- **レスポンス送信の信頼性**: `send_all()` で全バイト送信を保証し、失敗時は接続を閉じる。
- **FastCGI プロトコル**: 128 バイト以上のパラメータ長に対応した 4 バイトエンコーディングを実装。params が 65535 バイトを超える場合は複数レコードに分割。応答パースは動的バッファで安全に行う。
- **FastCGI 応答の安全性**: `FCGI_END_REQUEST` レコードを必須とし、応答全体の上限（64MiB）とレコード数上限（100,000）を設けて無限ループ・メモリ枯渇を防止。上流エラー時は `502` を返す。
- **環境変数のサニタイズ**: FastCGI 環境変数の name/value から `\r` `\n` `\0` を含むものを拒否し、ヘッダーインジェクションと C 文字列切り詰めを防止。
- **FCGI 構造体のパック**: `#pragma pack(push, 1)` と `static_assert` で `FCGI_Header` 等のサイズを保証。
- **chdir の RAII**: `invoke_fastcgi()` 内の `chdir()` は `CwdGuard` でスコープ退出時に必ず復元される（早期 return / 例外安全）。
- **子プロセス管理**: `execvp()` 失敗時に `_exit(127)`、`SIGCHLD` でゾンビ防止、デストラクタで `SIGTERM` 送信。`launch()` 失敗時は早期リターン。
- **WebSocket**: RFC 6455 準拠のフレーム解析を実装。拡張ペイロード長、断片化、ping/pong/close に対応。最大メッセージサイズは 1MiB。プロトコルエラー時は Close フレーム（status 1000）を送信。ログ出力時はペイロードの制御文字をサニタイズ。
- **Unix ソケットのクリーンアップ**: `app/myfcgi.c` の `serv_unix_socket()` は、正常終了時および `SIGTERM`/`SIGINT` 受信時に Unix ドメインソケットファイルを削除する。これにより、プロセス終了後のソケットファイル残存による次回起動時の `bind` エラーを防ぐ。
- **MCP (`/mcp`) セッション管理**: セッション状態（`MyHandler::McpSession`/`mcp_sessions_`）は `MyHandler` のメンバとし `std::mutex`（`mcp_sessions_mutex_`）で保護（`MyHandler` インスタンスは全ワーカースレッドで共有される単一オブジェクトのため）。セッション ID は `uuidv7()` の全ビット（CSPRNG 由来）を 32 桁 hex にエンコードして生成し、`initialize` 時に必ずサーバー側で新規発行する（クライアント指定 ID は使わずセッション固定化を防止）。セッションは 30 分未アクセスで失効、最大件数（`MCP_MAX_SESSIONS`）を超えると新規作成を `503` で拒否してメモリ枯渇を防ぐ。クライアント指定の `mcp-session-id` は hex かつ長さ上限内かを検証してから使用する。`Origin` ヘッダーを検証し `localhost`/`127.0.0.1`/`::1` 以外からのクロスオリジンリクエストは `403` で拒否（DNS rebinding 対策、`Origin` 自体が無いリクエストは非ブラウザクライアントとして許可）。JSON-RPC の `id`（数値/文字列/null）は型を保持したまま応答に書き戻し、不正なリクエスト・未知の method・`tools/call` の引数不正は HTTP 200 のまま JSON-RPC エラーオブジェクト（`-32600`/`-32601`/`-32602`）として返す。`uuidv7()` が使うグローバル CSPRNG（`misc/uuid.cpp` の `ChaCha20 rng`）はマルチスレッドから呼ばれるため `std::mutex` で保護している。

## Streamable HTTP / SSE をソケットで実装する際のハマりポイント

`/mcp` エンドポイント（`serv/main.cpp` の `do_get`/`do_post` 内、`path == "/mcp"` の分岐）は、MCP の Streamable HTTP トランスポートを、HTTP ライブラリを介さず `response->write()` で生バイト列を組み立てて実装している。ヘッダーもチャンクも SSE フレームも区別なく `http_response_t::content`（`std::vector<char>` 1本）に連結されるため（`serv/httpserver.h` の `http_response_t::write_()`）、正しさは完全に「`\r\n` をどこに何個置くか」「宣言した長さと実バイト数が一致しているか」に依存する。ここで実際に踏んだ／踏みかねない罠を整理する。

### 1. ヘッダーとボディの境界は「本当に空の行」でしか判定されない

- `parse_header()`（`serv/httpserver.cpp:216-247`）は `response.content` を先頭から走査し、行末の `\r`/`\n` を取り除いたあとに **完全に空になった最初の行** をヘッダー終端とみなす。それより前が `header`、それより後ろがボディ（`len = end - ptr`）になる。
- 行末以外の空白（例えば `" \r\n"` のように改行の直前にスペースが残っている行）はトリム対象外なので「空行」と判定されず、ヘッダー解析が終わらない。想定していたボディの先頭行がヘッダーとして飲み込まれ、`:` を含まない行は `http_send_response_header()` 側で黙って捨てられる（`serv/httpserver.cpp:838-840` の `find(':') == npos` continue）。
- **`response->write("\r\n")` を1回書く＝「追加ヘッダーなし」のつもりで使うのは危険**。`content` が空でなくなるため、4xx 応答時の自動エラーボディ生成（`serv/httpserver.cpp:580-587`、`content.empty()` のときだけ `Content-Type: text/plain` とステータステキストを補完する）が効かなくなる。実例: `serv/main.cpp:1103-1106` の `GET /mcp` は `response->write("\r\n"); return http400_bad_request;` としているため、他の 4xx 応答と違って `Content-Type` もボディも無い `Content-Length: 0` だけの 400 になる。自動生成のエラーボディに任せたいなら `response` に一切書き込まない。何か書くなら応答全体を自分で組み立てる覚悟が要る。

### 2. ヘッダー行はすべて「区切りの空行」より前でまとめて書く

- 上記の理由により、`write("X: Y\r\n")` は全部、区切りの `"\r\n"` を書く前に完了させる必要がある。ヘッダー書き込みとボディ／イベント書き込みを行ったり来たりする、あるいは空行を早く書きすぎると、残りのヘッダーが本文側に漏れる。
- `tools/list`/`tools/call`（`serv/main.cpp:1372-1375`, `1405-1408`）は `mcp-session-id` ヘッダー→空行→`event:`/`data:` の順で正しい。`initialize`（`serv/main.cpp:1298-1315`）は `mcp-session-id`→`Transfer-Encoding: chunked`→空行→チャンク本体、と1行ヘッダーが増える分だけ順序に注意が必要になる。

### 3. Chunked Transfer-Encoding は「長さ先出し」であり、行区切りではない

- 1チャンクの形式は `<16進サイズ>\r\n<サイズ分の生データ>\r\n`。ストリーム終端は `0\r\n\r\n`（サイズ0のチャンク＋区切りの空行で、CRLF が2つ必要）。
- 宣言する16進サイズは、直後に続く**データ本体のバイト数のみ**で、末尾の `\r\n` は含めない。ここを2バイト分ずれさせると（末尾の `\r\n` を数に入れる／データの後ろに `\r\n` を付け忘れる）、以降のチャンク境界が全部ズレる。行指向のバグと違って「その行だけ壊れる」のではなく、**宣言長で読み進める側の解析がそこから全部狂う**（`serv/httpserver.cpp:623-658` のチャンク再走査ロジックは、サイズどおりに読み飛ばして次のチャンクヘッダー位置を決めているため、ズレは伝播する）。
- サイズは「最終的に組み上がった1本のバッファ」から取ること。`serv/main.cpp:1300-1312` は `data`（`event:`/`data:` の SSE フレーム全体）を先に完成させてから `data.size()` を測り、その16進値→`data` 本体、の順で書いている。サイズを測ったあとにバッファへ追記する、といった順序にしない。
- サイズはバイト数であり文字数ではない（`std::string::size()` なら問題ないが、UTF-8 を文字単位で数える処理に差し替えると壊れる）。
- `Transfer-Encoding: chunked` と `Content-Length` は共存できない。このサーバーはヘッダー値が厳密に小文字の `"chunked"` と一致した場合のみ `Content-Length` の付与を止める（`serv/httpserver.cpp:602-606`）。`"Chunked"` や `"gzip, chunked"` のような値だと素通りしてしまい、チャンク化された本体に `Content-Length` まで付いてしまう（プロトコル違反／リクエストスマグリングと同じ構造の不整合）。生成する側は常に厳密に小文字 `chunked` のみを書くこと。
- 自前のチャンク再走査パーサー（`serv/httpserver.cpp:628-637`）はチャンク拡張（`<size>;ext=value\r\n`）を理解しない。`sprintf(tmp, "%x\r\n", ...)`（`serv/main.cpp:1309`）のように拡張なしの素の16進＋`\r\n`だけを生成すること。

### 4. SSE のイベント境界も「改行の本数と位置」で決まる

- SSE の1フィールド行は改行1個で終わる。**空行が続いたときだけイベントとして確定する**。`"data:" + json + "\r\n\r\n"`（`serv/main.cpp:1302`）の2個の `\r\n` は、1個目が `data:` 行の終端、2個目が区切りの空行という別の役割を持つ。1個しか書かなければクライアントは後続フィールドを待ち続け、3個以上書けば無駄な空イベントが増える。
- `data:` の値そのものに `\n`/`\r` を含めてはいけない。含む場合は SSE 仕様上、値の各行ごとに `data:` を付け直す必要があり、そうしないと途中の改行でフィールドが早期に終端したものとして誤解釈される。MCP の JSON レスポンスを組み立てる際に `w.enable_newline(false)`（`serv/main.cpp:1271-1272`, `1328-1329`, `1386-1387`）で常に1行の JSON にしているのはこのため。JSON 整形にインデント・改行を戻すと SSE フレームが壊れる。
- このコードは `"data:" + json` とコロンの直後にスペースを入れていない（SSE 仕様上はスペース1個までは許容され省略も可）。慣習を統一しておかないと、後から手書きでフレームを足したときに `"data: "` と混在し、うっかり JSON 側にスペースが混入して壊れる、という事故を招きやすい。

## 作業履歴

### WebSocket テストページ追加＋サーバー側不具合修正

ブラウザによる WebSocket 動作確認を目的として `static/ws.html`（エコーテスト UI）を追加した際に、複数の既存不具合が判明したため合わせて修正した。

**変更ファイルと内容**

| ファイル | 変更内容 |
|---|---|
| `fcgiapp.pro`, `fcgiserv.pro` | C++11 → C++17 (`std::string_view` / `std::optional` に対応)。 |
| `static/ws.html` | 新規作成。`ws://host/sock` に接続し、テキストメッセージの送受信とログ表示ができる WebSocket テストページ。 |
| `serv/socket.h` | `set_tcp_nodelay()` を追加（Nagle アルゴリズム無効化）。 |
| `serv/httpserver.cpp` | - `accept()` 直後に `set_tcp_nodelay()` を呼び出し、小さなフレーム（101 レスポンス、WS 制御フレーム）を即座に送信するように変更。<br>- WebSocket 受信メッセージをエコー返信（受信内容をそのまま返す）に変更。<br>- エコー返信時にも `printlog` を追加して送受信を記録。<br>- `101 Switching Protocols` レスポンスでは `Content-Length` ヘッダーおよびレスポンスボディ (`send_all`) を送信しないように修正。ブラウザがハンドシェイクを拒否していた原因を解消。<br>- `HTTP_Server::run()` の worker スレッド数を **1 → 8** に増加。シングルスレッドでは `keep-alive` 接続中に新規接続がブロックしていたため。 |
| `serv/main.cpp` | - `serve_static_file()` で `/static/` プレフィックス除去のコメントアウトを解除。`/static/...` パスが `404` になっていたのを修正。<br>- WebSocket ハンドシェイクの `Connection` ヘッダー値を RFC 7230 準拠の `Upgrade` に修正（元は小文字の `upgrade`）。 |
| `serv/debug.cpp` | Linux 版 `printlog()` で `stderr` にも `[fcgiserv] ...` 形式で出力するように変更。サーバーをフォアグラウンド起動したターミナルでリアルタイムにログを確認可能。 |

**修正した不具合の詳細**

1. **WebSocket 接続確立に 1 分遅延**: `101 Switching Protocols` レスポンスが Nagle アルゴリズムによりバッファリングされていたため、ブラウザがハンドシェイク完了を待ち続けていた。`set_tcp_nodelay()` で即時送信に変更。
2. **ブラウザがハンドシェイクを拒否**: `101 Switching Protocols` レスポンスに `Content-Length: 0` が付与されていたため、ブラウザが不正なレスポンスと判断していた。101 ステータスでは `Content-Length` とボディ送信を行わないように変更。
3. **同一クライアントからの新規接続がブロック**: サーバーの worker スレッドが単一（1 本）だったため、前のリクエストの `keep-alive` 処理中に新しい WebSocket 接続を `accept()` できなかった。スレッド数を 8 に増加して解消。

## Git の状態に関する注意

`serv/main.cpp` 等に未コミットの変更があります。新たな編集前に `git status` / `git diff` を確認すること。
