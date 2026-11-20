#include "core/insertitems.h"

#include <QRegularExpression>
#include <algorithm>

namespace insertitems {

QString sectionLabel(const QString &section, bool zh) {
    if (section == QLatin1String("text")) return zh ? QStringLiteral("文本") : QStringLiteral("Text");
    if (section == QLatin1String("list")) return zh ? QStringLiteral("列表与待办") : QStringLiteral("Lists & to-dos");
    if (section == QLatin1String("time")) return zh ? QStringLiteral("日期与时间") : QStringLiteral("Dates & time");
    if (section == QLatin1String("block")) return zh ? QStringLiteral("块") : QStringLiteral("Blocks");
    if (section == QLatin1String("link")) return zh ? QStringLiteral("链接与文件") : QStringLiteral("Links & files");
    return section;
}

const QList<Item> &all() {
    static const QList<Item> items{
        // Text
        {QStringLiteral("h1"), QStringLiteral("text"), QStringLiteral("一级标题"), QStringLiteral("Heading 1"),
         QStringLiteral("大标题，开启一个章节"), QStringLiteral("Big heading for a section"), QStringLiteral("H1"), QStringLiteral("#"),
         {QStringLiteral("biaoti"), QStringLiteral("bt"), QStringLiteral("heading"), QStringLiteral("title"), QStringLiteral("#")}},
        {QStringLiteral("h2"), QStringLiteral("text"), QStringLiteral("二级标题"), QStringLiteral("Heading 2"),
         QStringLiteral("中标题"), QStringLiteral("Medium heading"), QStringLiteral("H2"), QStringLiteral("##"),
         {QStringLiteral("biaoti"), QStringLiteral("bt"), QStringLiteral("heading"), QStringLiteral("subtitle"), QStringLiteral("##")}},
        {QStringLiteral("h3"), QStringLiteral("text"), QStringLiteral("三级标题"), QStringLiteral("Heading 3"),
         QStringLiteral("小标题"), QStringLiteral("Small heading"), QStringLiteral("H3"), QStringLiteral("###"),
         {QStringLiteral("biaoti"), QStringLiteral("bt"), QStringLiteral("heading"), QStringLiteral("###")}},
        {QStringLiteral("bold"), QStringLiteral("text"), QStringLiteral("加粗"), QStringLiteral("Bold"),
         QStringLiteral("让文字更醒目"), QStringLiteral("Make text stand out"), QStringLiteral("B"), QStringLiteral("**"),
         {QStringLiteral("jiacu"), QStringLiteral("jc"), QStringLiteral("cuti"), QStringLiteral("strong"), QStringLiteral("**")}},
        {QStringLiteral("italic"), QStringLiteral("text"), QStringLiteral("斜体"), QStringLiteral("Italic"),
         QStringLiteral("倾斜，轻轻强调"), QStringLiteral("Slanted emphasis"), QStringLiteral("I"), QStringLiteral("*"),
         {QStringLiteral("xieti"), QStringLiteral("xt"), QStringLiteral("em"), QStringLiteral("emphasis")}},
        {QStringLiteral("strike"), QStringLiteral("text"), QStringLiteral("删除线"), QStringLiteral("Strikethrough"),
         QStringLiteral("划掉不再需要的内容"), QStringLiteral("Cross something out"), QStringLiteral("S"), QStringLiteral("~~"),
         {QStringLiteral("shanchuxian"), QStringLiteral("scx"), QStringLiteral("strike"), QStringLiteral("del"), QStringLiteral("~~")}},
        {QStringLiteral("highlight"), QStringLiteral("text"), QStringLiteral("高亮"), QStringLiteral("Highlight"),
         QStringLiteral("像荧光笔一样标记"), QStringLiteral("Mark it like a highlighter"), QStringLiteral("A"), QStringLiteral("=="),
         {QStringLiteral("gaoliang"), QStringLiteral("gl"), QStringLiteral("mark"), QStringLiteral("highlight"), QStringLiteral("==")}},
        {QStringLiteral("code"), QStringLiteral("text"), QStringLiteral("行内代码"), QStringLiteral("Inline code"),
         QStringLiteral("等宽的代码或命令"), QStringLiteral("A code word or command"), QStringLiteral("‹›"), QStringLiteral("`"),
         {QStringLiteral("daima"), QStringLiteral("dm"), QStringLiteral("code"), QStringLiteral("`")}},
        {QStringLiteral("quote"), QStringLiteral("text"), QStringLiteral("引用"), QStringLiteral("Quote"),
         QStringLiteral("引用一段话"), QStringLiteral("Quote a passage"), QStringLiteral("❝"), QStringLiteral(">"),
         {QStringLiteral("yinyong"), QStringLiteral("yy"), QStringLiteral("quote"), QStringLiteral("blockquote"), QStringLiteral(">")}},
        // Lists & to-dos
        {QStringLiteral("todo"), QStringLiteral("list"), QStringLiteral("待办"), QStringLiteral("To-do"),
         QStringLiteral("可以勾选完成的事项"), QStringLiteral("Something to check off"), QStringLiteral("☐"), QStringLiteral("- [ ]"),
         {QStringLiteral("daiban"), QStringLiteral("db"), QStringLiteral("renwu"), QStringLiteral("rw"), QStringLiteral("task"), QStringLiteral("todo"), QStringLiteral("checkbox"), QStringLiteral("[]")}},
        {QStringLiteral("bullet"), QStringLiteral("list"), QStringLiteral("无序列表"), QStringLiteral("Bulleted list"),
         QStringLiteral("简单的项目列表"), QStringLiteral("A simple list"), QStringLiteral("•"), QStringLiteral("-"),
         {QStringLiteral("liebiao"), QStringLiteral("lb"), QStringLiteral("wuxu"), QStringLiteral("wx"), QStringLiteral("list"), QStringLiteral("bullet"), QStringLiteral("ul"), QStringLiteral("-")}},
        {QStringLiteral("ordered"), QStringLiteral("list"), QStringLiteral("有序列表"), QStringLiteral("Numbered list"),
         QStringLiteral("带编号的步骤"), QStringLiteral("Numbered steps"), QStringLiteral("1."), QStringLiteral("1."),
         {QStringLiteral("liebiao"), QStringLiteral("lb"), QStringLiteral("youxu"), QStringLiteral("yx"), QStringLiteral("list"), QStringLiteral("numbered"), QStringLiteral("ol"), QStringLiteral("1.")}},
        // Dates & time
        {QStringLiteral("due"), QStringLiteral("time"), QStringLiteral("日期与提醒"), QStringLiteral("Date & reminder"),
         QStringLiteral("给待办设日期和重复，到点提醒"), QStringLiteral("Due date and repeat, with a reminder"), QStringLiteral("📅"), QStringLiteral("@"),
         {QStringLiteral("riqi"), QStringLiteral("rq"), QStringLiteral("tixing"), QStringLiteral("tx"), QStringLiteral("richeng"), QStringLiteral("rc"),
          QStringLiteral("date"), QStringLiteral("due"), QStringLiteral("remind"), QStringLiteral("reminder"), QStringLiteral("deadline"), QStringLiteral("jiezhi")}},
        {QStringLiteral("today"), QStringLiteral("time"), QStringLiteral("今天的日期"), QStringLiteral("Today's date"),
         QStringLiteral("插入今天的日期"), QStringLiteral("Insert the date of today"), QStringLiteral("🗓"), QString(),
         {QStringLiteral("jintian"), QStringLiteral("jt"), QStringLiteral("riqi"), QStringLiteral("rq"), QStringLiteral("today"), QStringLiteral("date")}},
        {QStringLiteral("now"), QStringLiteral("time"), QStringLiteral("当前时间"), QStringLiteral("Current time"),
         QStringLiteral("插入现在的日期和时间"), QStringLiteral("Insert the date and time"), QStringLiteral("🕘"), QString(),
         {QStringLiteral("shijian"), QStringLiteral("sj"), QStringLiteral("xianzai"), QStringLiteral("xz"), QStringLiteral("now"), QStringLiteral("time")}},
        // Blocks
        {QStringLiteral("codeblock"), QStringLiteral("block"), QStringLiteral("代码块"), QStringLiteral("Code block"),
         QStringLiteral("多行代码，保留原样"), QStringLiteral("Multi-line code, kept as is"), QStringLiteral("{}"), QStringLiteral("```"),
         {QStringLiteral("daimakuai"), QStringLiteral("dmk"), QStringLiteral("daima"), QStringLiteral("code"), QStringLiteral("codeblock"), QStringLiteral("```")}},
        {QStringLiteral("table"), QStringLiteral("block"), QStringLiteral("表格"), QStringLiteral("Table"),
         QStringLiteral("行和列，支持网格编辑与汇总"), QStringLiteral("Rows and columns, editable grid"), QStringLiteral("▦"), QStringLiteral("|"),
         {QStringLiteral("biaoge"), QStringLiteral("bg"), QStringLiteral("table"), QStringLiteral("grid"), QStringLiteral("|")}},
        {QStringLiteral("divider"), QStringLiteral("block"), QStringLiteral("分割线"), QStringLiteral("Divider"),
         QStringLiteral("一条横线，分开内容"), QStringLiteral("A line between sections"), QStringLiteral("—"), QStringLiteral("---"),
         {QStringLiteral("fengexian"), QStringLiteral("fgx"), QStringLiteral("divider"), QStringLiteral("hr"), QStringLiteral("line"), QStringLiteral("rule"), QStringLiteral("---")}},
        {QStringLiteral("properties"), QStringLiteral("block"), QStringLiteral("笔记属性"), QStringLiteral("Properties"),
         QStringLiteral("笔记开头的键值信息"), QStringLiteral("Key-value facts at the top"), QStringLiteral("⚙"), QStringLiteral("---"),
         {QStringLiteral("shuxing"), QStringLiteral("sx"), QStringLiteral("properties"), QStringLiteral("frontmatter"), QStringLiteral("yaml"), QStringLiteral("meta")}},
        // Links & files
        {QStringLiteral("wiki"), QStringLiteral("link"), QStringLiteral("链接到笔记"), QStringLiteral("Link to note"),
         QStringLiteral("链接到另一篇笔记"), QStringLiteral("Link to another note"), QStringLiteral("[["), QStringLiteral("[["),
         {QStringLiteral("shuanglian"), QStringLiteral("sl"), QStringLiteral("lianjie"), QStringLiteral("lj"), QStringLiteral("wiki"), QStringLiteral("link"), QStringLiteral("note"), QStringLiteral("[[")}},
        {QStringLiteral("link"), QStringLiteral("link"), QStringLiteral("网页链接"), QStringLiteral("Web link"),
         QStringLiteral("插入一个网址"), QStringLiteral("Insert a web address"), QStringLiteral("↗"), QString(),
         {QStringLiteral("lianjie"), QStringLiteral("lj"), QStringLiteral("wangzhi"), QStringLiteral("wz"), QStringLiteral("link"), QStringLiteral("url"), QStringLiteral("href")}},
        {QStringLiteral("image"), QStringLiteral("link"), QStringLiteral("网络图片"), QStringLiteral("Web image"),
         QStringLiteral("插入一张网络图片"), QStringLiteral("Insert an image from URL"), QStringLiteral("🖼"), QString(),
         {QStringLiteral("tupian"), QStringLiteral("tp"), QStringLiteral("zhaopian"), QStringLiteral("zp"), QStringLiteral("image"), QStringLiteral("photo"), QStringLiteral("picture"), QStringLiteral("img")}},
        {QStringLiteral("file"), QStringLiteral("link"), QStringLiteral("附件"), QStringLiteral("Attachment"),
         QStringLiteral("从本机选择图片或文件"), QStringLiteral("Pick an image or file from this device"), QStringLiteral("📎"), QString(),
         {QStringLiteral("fujian"), QStringLiteral("fj"), QStringLiteral("wenjian"), QStringLiteral("wj"), QStringLiteral("file"), QStringLiteral("attachment"), QStringLiteral("upload"), QStringLiteral("tupian"), QStringLiteral("tp")}},
    };
    return items;
}

QList<Item> match(const QString &query) {
    const QString q = query.trimmed().toLower();
    const auto &items = all();
    if (q.isEmpty()) return items;
    static const QRegularExpression nonWord(QStringLiteral(R"(\W+)"));
    struct Scored {
        int score;
        int order;
        Item item;
    };
    QList<Scored> hits;
    for (int i = 0; i < items.size(); ++i) {
        const Item &it = items[i];
        const QString zh = it.zh.toLower();
        const QString en = it.en.toLower();
        const QString id = it.id.toLower();
        int score = 0;
        if (zh == q || en == q || id == q) score = 100;
        else if (zh.startsWith(q) || en.startsWith(q) || id.startsWith(q)) score = 80;
        else if (std::any_of(it.keys.begin(), it.keys.end(), [&](const QString &k) { return k == q; })) score = 70;
        else if (std::any_of(it.keys.begin(), it.keys.end(), [&](const QString &k) { return k.startsWith(q); })) score = 60;
        else if (zh.contains(q) || en.contains(q)) score = 40;
        else {
            const QStringList words = en.split(nonWord, Qt::SkipEmptyParts);
            if (std::any_of(words.begin(), words.end(), [&](const QString &w) { return w.startsWith(q); })) score = 35;
        }
        if (score > 0) hits.append({score, i, it});
    }
    std::sort(hits.begin(), hits.end(), [](const Scored &a, const Scored &b) {
        return a.score != b.score ? a.score > b.score : a.order < b.order;
    });
    QList<Item> out;
    out.reserve(hits.size());
    for (const Scored &s : hits) out.append(s.item);
    return out;
}

} // namespace insertitems
