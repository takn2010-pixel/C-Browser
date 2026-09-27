# C-Browser 🚀

C言語、GTK+ 3、libcurl、gumbo-parser、libcss を用いて作られた、超軽量（？）自作テキストベースWebブラウザです。  
WebKitGTKなどの既存ブラウザエンジンに頼らず、HTML/CSSのパースからGtkTextBufferへのレンダリングまでを自前で処理しています。 

## 🌟 特徴
- **自前HTML/CSSパース**: `gumbo-parser` でDOMツリーを構築し、`libcss` でスタイル計算を実施
- **GtkTextViewでの描画**: CSSの `color`, `font-weight`, `font-size` を `GtkTextTag` にマッピング
- **インライン画像表示**: `<img>` タグの画像を非同期的に取得し、テキスト内に埋め込み描画
- **フォーム入力サポート**: ダブルクリックでテキスト入力ダイアログを起動し、GET/POST送信が可能
- **DuckDuckGo検索連携**: 検索バーから直接Web検索が可能

## ⚠️ 著作者
内部ではNOK.Tとなっています。
著作者は『takn2010-pixel( https://github.com/takn2010-pixel )』となります

## 📦 依存ライブラリ (Dependencies)
ビルドには以下のライブラリが必要です。

```bash
# Ubuntu / Debian の例
sudo apt install build-essential libgtk-3-dev libcurl4-openssl-dev libgumbo-dev libcss-dev libwapcaplet-dev
```

## 🛠 ビルド方法 (Build)

```bash
gcc -o c-browser browser.c `pkg-config --cflags --libs gtk+-3.0 libcurl gumbo libcss libwapcaplet`
```

## 🚀 実行

```bash
./c-browser
```
