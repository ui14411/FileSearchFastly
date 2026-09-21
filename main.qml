import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import FileSearchFastly 1.0

ApplicationWindow {
    id: window

    width: 1100
    height: 700
    visible: true
    minimumHeight:480
    minimumWidth:480
    title: "FileSearchFastly"

    //蓝灰色调色板
    color: "#eaf0f8"

    //搜索状态
    property int searchType: 0
    property int sortType: 5   
    property string searchKeyword: ""  
    property var lastResults: [] 
    property var displayResults: []  
    property var driveList: []  
    property string currentDrive: "全部"  
    property int searchTick: 0    
    property int loadingCount: 0   
    property int pageSize: 500 
    property int loadedCount: 0
    property string scanStatus: ""
    property string currentPath: ""   // 当前所在目录（文件夹模式导航，空=不在目录内）
    property string emptyHint: ""     // 空态提示（未找到 / 请输入关键词 / 此目录为空）
    property bool hasSearched: false  // 是否已执行过搜索（区分"还没搜"和"搜了没结果"）
    property bool hasMoreAll: false   // 全量模式下后端还有下一页（由 searchResultAll 的 hasMore 驱动）
    property bool sortBoxSync: false  // 程序内同步排序下拉时置真，避免多打一次查询

    // 启动时获取盘符列表
    Component.onCompleted: {
        window.driveList = fileInteract.getDrives()
        console.log("盘符列表:", JSON.stringify(window.driveList))
    }

    // FileInteract 信号连接 
    Connections {
        target: fileInteract

        // 接收文件夹内容（列目录）结果
        function onSearchResultByFolderContent(results) {
            console.log("文件夹内容，条目数:", results.length)
            window.requestShow(results, "此目录为空")
        }

        // 接收全量搜索结果（文件+文件夹）：过滤与排序已由 SQL 完成 → 直接填，不再走前端排序
        // 第二页起 append=true，往现有列表尾部追加，不能重建
        function onSearchResultAll(results, hasMore, append) {
            console.log("全量搜索完成，本页:", results.length, "还有更多:", hasMore, "追加:", append)
            window.hasMoreAll = hasMore
            if (append) {
                if (results.length === 0)
                    return
                // 就地 push 不触发属性变更通知，底部计数绑定不会刷新 → 整体赋值新数组
                window.displayResults = window.displayResults.concat(results)
                window.lastResults = window.displayResults
                window.loadMore()          // 只把新追加的这批喂进 model
                Qt.callLater(window.ensureViewportFilled)
                return
            }
            window.lastResults = results
            window.requestShow(results, undefined, true)
        }

        // 扫描状态栏
        function onScanStatusChanged(text) {
            console.warn("QML 收到扫描状态:", text)   // 诊断
            window.scanStatus = text
        }

        // 搜索完成信号
        function onSearchFinished(count, error) {
            if (error) {
                console.error("搜索错误:", error)
            } else {
                console.log("搜索完成，找到", count, "个结果")
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 15

        // 标题 + 扫描状态动态提示（同一行）
        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            Label {
                text: "FileSearchFastly"
                font.pixelSize: 28
                font.bold: true
                color: "#2c4059"             // 深蓝灰
            }

            // 扫描状态提示：USN 盘 → 加速；U盘 → 较慢；扫描完成 → 隐藏
            Text {
                id: scanHint
                // 绑定 Q_PROPERTY：QML 加载后自动读到当前扫描状态（信号在加载前 emit 也不丢）
                visible: fileInteract.scanStatusText.length > 0
                text: fileInteract.scanStatusText
                color: "#5d7f9e"
                font.pixelSize: 13
                verticalAlignment: Text.AlignVCenter
                opacity: fileInteract.scanStatusText.length > 0 ? 1 : 0
                Behavior on opacity { NumberAnimation { duration: 200 } }
            }

            Item { Layout.fillWidth: true }   // 占位：保持标题左对齐
        }

        // 搜索区域
        Rectangle {
            Layout.fillWidth: true
            height: 60
            radius: 10
            color: "#ffffff"
            border.color: "#b8cbe0"      // 蓝灰边框
            border.width: 1

            RowLayout {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 10

                ComboBox {
                    id: searchTypeBox
                    Layout.preferredWidth: 130
                    model: ["文件", "文件夹", "文件后缀"]
                    onCurrentIndexChanged: {
                        window.searchType = currentIndex
                        window.currentPath = ""   // 换模式即离开目录导航，回到搜索结果
                        // 文件夹模式默认按最后修改时间排序
                        if (currentIndex === 1) {
                            window.sortType = 5
                            window.sortBoxSync = true
                            sortBox.currentIndex = 5
                            window.sortBoxSync = false
                        }
                        // 换模式 = 换一条查询，不再在前端过滤已加载的那批行
                        window.runSearch()
                    }
                }

                TextField {
                    id: searchEdit
                    Layout.fillWidth: true
                    placeholderText: {
                        switch (window.searchType) {
                        case 0: return "输入文件名..."
                        case 1: return "输入文件夹名称..."
                        case 2: return "输入后缀，例如 exe..."
                        default: return "搜索..."
                        }
                    }
                    font.pixelSize: 16
                    selectByMouse: true
                    Keys.onReturnPressed: searchButton.clicked()
                }

                Button {
                    id: searchButton
                    text: "搜索"
                    Layout.preferredWidth: 100
                    Layout.fillHeight: true
                    background: Rectangle {
                        radius: 6
                        color: searchButton.pressed ? "#4c6a8a" : "#5d7f9e"   // 蓝灰按钮
                    }
                    contentItem: Text {
                        text: searchButton.text
                        color: "white"
                        font.pixelSize: 14
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    onClicked: {
                        var kw = searchEdit.text.trim()
                        window.searchKeyword = kw
                        if (kw === "") {
                            window.showEmpty("请输入搜索关键词")
                            return
                        }
                        // 标记为已搜索：靠这个标志区分"还没搜过"和"搜了没结果"
                        window.hasSearched = true
                        window.currentPath = ""   // 新搜索不在任何目录里
                        window.runSearch()
                    }
                }
            }
        }

        //工具栏
        RowLayout {
            Layout.fillWidth: true
            height: 40

            Label {
                text: window.currentPath === "" ? "搜索结果" : window.currentPath
                font.pixelSize: 18
                font.bold: true
                color: "#2c4059"
                Layout.fillWidth: true
                elide: Text.ElideMiddle
            }

            // 返回上级按钮（目录导航：进入目录后显示）
            // 回到的是 UI 的上一层 = 搜索结果页，不是文件系统的父目录。
            // 目录里只可能从搜索结果点进来，所以上一层恒为搜索结果：
            // 清空 currentPath 后重跑当前模式的查询即可（runSearch 按 searchType 分派，
            // 三种模式都回到各自的结果页，同时顺手复位后端游标 → 第 1 页）
            Button {
                id: backBtn
                text: "⬅ 返回搜索结果"
                visible: window.currentPath !== ""
                onClicked: {
                    window.currentPath = ""
                    // 关键词被清空过就别重查了（runSearch 会直接 return，标题会和列表对不上）
                    if (window.searchKeyword !== "")
                        window.runSearch()
                    else
                        window.showEmpty("请输入搜索关键词")
                }
            }

            // 盘符过滤
            RowLayout {
                spacing: 4
                Repeater {
                    model: ["全部"].concat(window.driveList)
                    delegate: Rectangle {
                        width: Math.max(40, textItem.width + 14)
                        height: 26
                        radius: 4
                        color: window.currentDrive === modelData ? "#5d7f9e" : "#e3ebf4"
                        border.color: window.currentDrive === modelData ? "#5d7f9e" : "#c9d8e8"
                        border.width: 1
                        Text {
                            id: textItem
                            anchors.centerIn: parent
                            text: modelData === "全部" ? "全部" : modelData + " 盘"
                            color: window.currentDrive === modelData ? "white" : "#3d566e"
                            font.pixelSize: 12
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                window.currentDrive = modelData
                                window.runSearch()   // 换盘符 = 重查第 1 页（原来只在前端过滤已加载的那批行）
                            }
                        }
                    }
                }
            }

            Label {
                text: "排序："
                color: "#3d566e"
            }

            ComboBox {
                id: sortBox
                Layout.preferredWidth: 150
                model: ["名称 ↑", "名称 ↓", "大小 ↑", "大小 ↓", "修改时间 ↑", "修改时间 ↓"]
                currentIndex: 5   // 默认"修改时间 ↓"（与 window.sortType 初始值一致）
                // 排序下推在 SQL 的 ORDER BY 里：换档位要重查第 1 页才生效（重查同时复位游标，翻页照常）
                onCurrentIndexChanged: {
                    window.sortType = currentIndex
                    if (!window.sortBoxSync)
                        window.runSearch()
                }
            }
        }

        //文件列表
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: 10
            color: "#ffffff"
            border.color: "#b8cbe0"
            border.width: 1

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                // 表头
                Rectangle {
                    Layout.fillWidth: true
                    height: 45
                    color: "#d5e0ec"

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 15
                        anchors.rightMargin: 15

                        // 名称 / 大小 / 修改时间 可点击排序（点同列翻方向）
                        Item {
                            Layout.preferredWidth: 300
                            Layout.fillHeight: true
                            Label {
                                anchors.fill: parent
                                text: "名称"; font.bold: true; color: "#1f3347"
                                verticalAlignment: Text.AlignVCenter
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: window.toggleSort(0)
                            }
                        }
                        Label { text: "类型"; font.bold: true; color: "#1f3347"; Layout.preferredWidth: 100 }
                        Item {
                            Layout.preferredWidth: 120
                            Layout.fillHeight: true
                            Label {
                                anchors.fill: parent
                                text: "大小"; font.bold: true; color: "#1f3347"
                                verticalAlignment: Text.AlignVCenter
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: window.toggleSort(1)
                            }
                        }
                        Item {
                            Layout.preferredWidth: 180
                            Layout.fillHeight: true
                            Label {
                                anchors.fill: parent
                                text: "修改时间"; font.bold: true; color: "#1f3347"
                                verticalAlignment: Text.AlignVCenter
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: window.toggleSort(2)
                            }
                        }
                        Label { text: "路径"; font.bold: true; color: "#1f3347"; Layout.fillWidth: true }
                    }
                }

                // 下拉进度条
                Rectangle {
                    id: loadingBar
                    Layout.fillWidth: true
                    Layout.preferredHeight: window.loadingCount > 0 ? 22 : 0
                    Behavior on Layout.preferredHeight { NumberAnimation { duration: 200 } }
                    clip: true
                    color: "#eef3fa"
                    border.color: "#d5e0ec"
                    border.width: 1

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        spacing: 10
                        visible: window.loadingCount > 0

                        Text {
                            text: "加载中..."
                            color: "#5d7f9e"
                            font.pixelSize: 12
                        }

                        Rectangle {
                            id: loadingTrack
                            Layout.fillWidth: true
                            height: 4
                            radius: 2
                            color: "#d5e0ec"

                            Rectangle {
                                id: loadingSlider
                                width: 120
                                height: 4
                                radius: 2
                                color: "#5d7f9e"
                                NumberAnimation on x {
                                    running: window.loadingCount > 0
                                    from: -loadingSlider.width
                                    to: loadingTrack.width
                                    duration: 800
                                    loops: Animation.Infinite
                                }
                            }
                        }

                        Text {
                            text: window.loadedCount + " / " + (window.displayResults ? window.displayResults.length : 0)
                            color: "#8aa0b8"
                            font.pixelSize: 12
                        }
                    }
                }

                // 滚动
                ListView {
                    id: fileList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: ListModel { id: fileModel }

                    // 垂直滚动条（始终显示，可拖拽快速浏览）
                    ScrollBar.vertical: ScrollBar {
                        id: vScrollBar
                        policy: ScrollBar.AlwaysOn
                        width: 12
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom

                        // 滑块（蓝灰色调，与 UI 风格一致）
                        contentItem: Rectangle {
                            implicitWidth: 12
                            radius: 6
                            color: vScrollBar.pressed ? "#5d7f9e" : "#b8cbe0"
                            // 高度/位置跟随 ScrollBar 的 size/position（修复 undefined 警告）
                            height: Math.max(30, (vScrollBar.size || 1) * vScrollBar.height)
                            y: (vScrollBar.position || 0) * vScrollBar.height
                            Rectangle {
                                anchors.fill: parent
                                radius: 6
                                color: "transparent"
                                border.color: vScrollBar.hovered ? "#5d7f9e" : "transparent"
                                border.width: 1
                            }
                        }
                        // 轨道（淡灰色）
                        background: Rectangle {
                            implicitWidth: 12
                            radius: 6
                            color: "#eaf0f8"
                        }
                    }
                    onContentYChanged: {
                        if (contentY >= contentHeight - height - 300)
                            window.loadMore()
                    }
                    onAtYEndChanged: if (atYEnd) window.loadMore()
                    footer: Item {
                        width: fileList.width
                        height: 40
                        Text {
                            anchors.centerIn: parent
                            text: window.loadedCount < window.displayResults.length
                                  ? "↓ 下拉加载更多..."
                                  : "已全部加载（共 " + window.displayResults.length + " 条）"
                            color: "#8aa0b8"
                            font.pixelSize: 13
                        }
                    }

                    delegate: Rectangle {
                        width: fileList.width
                        height: 55
                        color: mouseArea.containsMouse ? "#d7e5f5" : "#ffffff"   // 悬停蓝灰

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 15
                            anchors.rightMargin: 15
                            spacing: 10

                            Label {
                                text: model.name
                                elide: Text.ElideRight
                                color: "#1f3347"
                                Layout.preferredWidth: 300
                            }
                            Label {
                                text: model.isFolder ? "文件夹" : String(model.suffix || "").toUpperCase()
                                color: "#3d566e"
                                Layout.preferredWidth: 100
                            }
                            Label {
                                text: model.isFolder ? "-" : formatSize(model.size)
                                color: "#3d566e"
                                Layout.preferredWidth: 120
                            }
                            Label {
                                text: model.modifiedTime
                                color: "#3d566e"
                                Layout.preferredWidth: 180
                            }
                            Label {
                                text: model.path
                                elide: Text.ElideMiddle
                                color: "#5a6f85"
                                Layout.fillWidth: true
                            }
                        }

                        MouseArea {
                            id: mouseArea
                            anchors.fill: parent
                            hoverEnabled: true
                            acceptedButtons: Qt.LeftButton

                            // 单击文件夹：延迟 250ms 判断（防双击冲突）后进入目录（cd）
                            Timer {
                                id: clickTimer
                                interval: 250
                                repeat: false
                                onTriggered: {
                                    if (("" + model.isFolder) === "true") {
                                        window.currentPath = model.path
                                        fileInteract.searchByFolderContent(model.path)
                                    }
                                }
                            }
                            onClicked: {
                                if (("" + model.isFolder) === "true")
                                    clickTimer.restart()
                            }
                            onDoubleClicked: {
                                clickTimer.stop()
                                console.log("打开:", model.path)
                                // 在资源管理器中定位该文件
                                fileInteract.showInExplorer(model.path)
                            }
                        }
                    }
                }
            }

            // 空态提示：未找到 / 请输入关键词 / 此目录为空
            Text {
                anchors.centerIn: parent
                width: parent.width - 80
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                visible: window.emptyHint !== "" && window.loadingCount === 0
                text: window.emptyHint
                color: "#8aa0b8"
                font.pixelSize: 15
            }
        }

        // 底部状态
        RowLayout {
            Layout.fillWidth: true
            Label {
                text: "结果：" + (window.displayResults ? window.displayResults.length : 0) + " 个"
                color: "#5a6f85"
            }
            Item { Layout.fillWidth: true }
            Label {
                text: "FileSearchFastly"
                color: "#8aa0b8"
            }
        }
    }

    // 表头点击排序：同列再点翻方向，换列默认降序。
    // col：0=名称 1=大小 2=修改时间（与 sortType/2 的后端列号一致）
    function toggleSort(col) {
        if (Math.floor(window.sortType / 2) === col)
            window.sortType = col * 2 + (1 - window.sortType % 2)
        else
            window.sortType = col * 2 + 1
        window.sortBoxSync = true          // 同步下拉框但不触发它自己那次重查
        sortBox.currentIndex = window.sortType
        window.sortBoxSync = false
        window.runSearch()
    }

    // 用当前的 关键词 / 模式 / 排序档位 / 盘符 重新发一次查询。
    // 排序与过滤已下推 SQL，所以让设置生效 = 重查第 1 页；重发查询同时会把后端游标复位，
    // 因此换排序、切盘符、切模式之后都能继续正常往下翻页。
    function runSearch() {
        if (window.searchKeyword === "")
            return
        // 必须在使用前声明：原先写在 else 分支里，var 提升后前两个模式拿到 undefined，盘符过滤恒为空
        var drivePrefix = window.currentDrive === "全部" ? "" : window.currentDrive + ":\\"
        if (window.searchType === 2)
            fileInteract.searchBySuffix(window.searchKeyword,window.sortType, drivePrefix)
        else if (window.searchType === 1) {
            if (window.currentPath !== "")
                fileInteract.searchByFolderContent(window.currentPath)   // 在目录里 → 重列当前目录
            else
                fileInteract.searchByFolder(window.searchKeyword, window.sortType, drivePrefix)
        } else {
            fileInteract.searchAll(window.searchKeyword, window.sortType, drivePrefix)
        }
    }

    // 所有模式的结果都从这里进（文件名 / 文件夹 / 后缀 / 列目录）→ 空判断只写一处
    // 排序已全部交给 SQL 的 ORDER BY，两条分支都直接填充，前端不再重排
    // sorted = true：来自分页链路（后端驱动 hasMoreAll，不能清）；否则是前端过滤/裁剪过的视图
    function requestShow(results, emptyMsg, sorted) {
        if (!results || results.length === 0) {
            showEmpty(emptyMsg || (window.searchKeyword !== ""
                ? "未找到与 \"" + window.searchKeyword + "\" 匹配的文件"
                : "未找到文件"))
            return
        }
        window.hasSearched = true
        window.emptyHint = ""   // 有结果 → 清掉提示
        if (sorted) {
            searchTick++        // 作废在途的旧请求
            fillModel(results)
            return
        }
        window.hasMoreAll = false   // 走前端过滤的视图不分页（列表已被裁剪，后端游标对不上）
        searchTick++
        fillModel(results)
    }

    // 空态：清空列表 + 显示提示。
    // 必须推进 searchTick：上一次搜索还在途的排序结果回来时 seq 对不上会被丢弃，
    // 否则它会把刚清空的列表重新填满、盖住提示。
    function showEmpty(msg) {
        window.hasSearched = true
        window.emptyHint = msg
        displayResults = []
        fileModel.clear()
        loadedCount = 0
        hasMoreAll = false   // 列表都清空了，别让 loadMore 再去要下一页
        searchTick++
    }

    // 结果
    function fillModel(results) {
        displayResults = results
        fileModel.clear()
        loadedCount = 0
        loadMore()
        Qt.callLater(ensureViewportFilled)
    }

    function loadMore() {
        if (loadedCount >= displayResults.length) {
            if (window.hasMoreAll)
                fileInteract.loadNextPage()   // 本页切完了，后端还有 → 要下一页
            return
        }
        var end = Math.min(loadedCount + pageSize, displayResults.length)
        for (var i = loadedCount; i < end; i++) {
            var it = displayResults[i]
            fileModel.append({
                name: "" + it.name,
                path: "" + it.path,
                suffix: "" + (it.suffix || ""),
                size: Number("" + (it.size || 0)),
                modifiedTime: "" + it.modifiedTime,
                isFolder: ("" + it.isFolder) === "true"
            })
        }
        loadedCount = end
        console.log("加载更多: 已加载 " + loadedCount + " / " + displayResults.length)
    }

    // 首屏不足时自动补载，直到填满视口或全部加载完
    function ensureViewportFilled() {
        if (fileList.contentHeight < fileList.height && loadedCount < displayResults.length) {
            loadMore()
            Qt.callLater(ensureViewportFilled) 
        }
    }

    //工具函数 
    function formatSize(size) {
        if (size < 1024) return size + " B"
        if (size < 1024 * 1024) return (size / 1024).toFixed(2) + " KB"
        if (size < 1024 * 1024 * 1024) return (size / 1024 / 1024).toFixed(2) + " MB"
        return (size / 1024 / 1024 / 1024).toFixed(2) + " GB"
    }
}