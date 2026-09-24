# 開発とテスト

C17 で実装します。アプリケーションにもテストにも Python / C++ は必要ありません。GTK 3 の公開 API、libcurl、libxml2、SQLite、json-c を使います。依存ライブラリーのソースやバイナリー、フォントは同梱しません。

## 手元での確認

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/feedman_tests --benchmark
```

`xvfb-run` が見つかるデスクトップビルドでは `gui` テストを追加します。GUIテストは実際の GTK ウィジェットと非同期コールバックを使い、日付移動、検索、ページ送り、タグ、配置、文字サイズ、保存状態、処理中の終了を検証します。テスト専用フックは `feedman_ui_tests` にだけ組み込み、通常の `feedman` には含めません。

ネットワークテストは C で書いたループバック HTTP サーバーを起動します。外部サイト、個人の設定、実際の記事は不要です。実行環境がローカルソケット自体を禁止している場合は、その制約を解除できる環境でテストしてください。

```sh
cmake -S . -B build-asan -DFEEDMAN_GUI=OFF \
  -DFEEDMAN_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan --parallel
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build-asan --output-on-failure
```

GUIもASanで実行できますが、GTK/Fontconfigなどプロセス終了まで保持する外部ライブラリーの割り当てがリークとして報告される場合があります。検証結果では、アプリケーションのメモリー検査と外部ライブラリーのキャッシュを区別してください。

## 変更時の原則

GUI スレッドでネットワーク取得しないこと、SQLite ハンドルと CURL ハンドルを同時に複数スレッドで使わないこと、各文字列の所有者を明確にすることを守ってください。公開ヘッダーの文字列は、特記がなければ対応する `*_free` で解放します。GTK へ渡す文字列はコピーされる API と、コールバックまで保持が必要な API を区別します。

データベーススキーマを変更するときは `user_version` と移行手順を設計してください。既存DBを無条件に初期化・削除しないでください。現行コードは新しいスキーマ番号のDBを拒否します。

コードのスタイルは `.clang-format` を基準に整えられます。バグ修正には可能な限り再現用フィクスチャーとテストを付け、例示には `example.com` と架空の記事を使ってください。HTML、XML、URLを信頼済みコードとして扱わないでください。

## Pull request

目的、変更点、実行したテスト、未確認の環境を記載してください。速度改善ではデータ件数・遅延条件・スレッド数・ビルド条件を揃え、出力件数の一致も確認します。単発のネットワーク速度を実装差として扱わないでください。

GitHub Actionsは通常のpush / pull_requestで動き、公開・リリース・秘密情報へのアクセスは行いません。ローカルで作ったZIPにワークフローを入れることと、GitHub上でそのワークフローが成功したことは別です。
