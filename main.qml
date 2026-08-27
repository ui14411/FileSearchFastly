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

    // 进度条
    function beginLoad() { loadingCount++ }
    function endLoad() { if (loadingCount > 0) loadingCount-- }

    // 启动时获取盘符列表
    Component.onCompleted: {
        window.driveList = fileInteract.getDrives()
        console.log("盘符列表:", JSON.stringify(window.driveList))
    }

    // FileInteract 信号连接 
    Connections {
        target: fileInteract

        // 接收按后缀搜索的结果
        function onSearchResultBySuffix(results) {
            console.log("按后缀搜索完成，结果数:", results.length)
            window.lastResults = results
            window.applyModeFilter()   // 盘符过滤 + 后缀精确二次过滤
        }

        // 接收按文件名搜索的结果
        function onSearchResultByFile(results) {
            console.log("按文件名搜索完成，结果数:", results.length)
            window.requestShow(results)
        }

        // 接收按文件夹名搜索的结果：列出匹配文件夹，单击进入（cd），不再自动进第一个
        function onSearchResultByFolder(results) {
            console.log("匹配文件夹数:", results.length)
            window.currentPath = ""   // 搜索时重置路径（不在目录内）
            window.requestShow(results)
        }

        // 接收文件夹内容（列目录）结果
        function onSearchResultByFolderContent(results) {
            console.log("文件夹内容，条目数:", results.length)
            window.requestShow(results)
        }

        // 接收全量搜索结果（文件+文件夹）：存结果 → 按当前模式过滤显示
        function onSearchResultAll(results) {
            console.log("全量搜索完成，结果数:", results.length)
            window.lastResults = results
            window.applyModeFilter()
        }

        // 扫描状态栏
        function onScanStatusChanged(text) {
            console.warn("QML 收到扫描状态:", text)   // 诊断
            window.scanStatus = text
        }

        // 异步排序完成（线程池）：先消进度计数（每个 requestSort 必有一次 ready），
        // seq 对不上 = 过期结果，丢弃
        function onSortResultReady(sorted, seq) {
            window.endLoad()
            if (seq !== window.searchTick)
                return
            window.fillModel(sorted)
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
                        // 文件夹模式默认按最后修改时间排序
                        if (currentIndex === 1) {
                            window.sortType = 5
                            sortBox.currentIndex = 5
                        }
                        // 切换模式 = 前端过滤当前结果
                        window.applyModeFilter()
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
                        if (kw === "")
                            return
                        // 按模式分派：后缀走索引查询；文件夹走文件夹匹配；默认全量
                        if (window.searchType === 2)
                            fileInteract.searchBySuffix(kw)
                        else if (window.searchType === 1)
                            fileInteract.searchByFolder(kw)
                        else
                            fileInteract.searchAll(kw)
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

            // 返回上级按钮（目录导航：进入目录后显示，回父目录）
            Button {
                id: backBtn
                text: "⬆ 返回上级"
                visible: window.currentPath !== ""
                onClicked: {
                    var p = window.currentPath
                    var idx = p.lastIndexOf("\\")
                    if (idx > 2) {
                        // 非盘根：回父目录
                        window.currentPath = p.substring(0, idx)
                        fileInteract.searchByFolderContent(window.currentPath)
                    } else {
                        // 盘根：回到搜索（重新列出匹配文件夹）
                        window.currentPath = ""
                        fileInteract.searchByFolder(window.searchKeyword)
                    }
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
                                window.applyModeFilter()
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
                onCurrentIndexChanged: {
                    window.sortType = currentIndex
                    window.reapplySort()   // 即时重排当前结果
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

                        Label {
                            text: "名称"; font.bold: true; color: "#1f3347"; Layout.preferredWidth: 300
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: window.cycleSort(0)
                            }
                        }
                        Label { text: "类型"; font.bold: true; color: "#1f3347"; Layout.preferredWidth: 100 }
                        Label {
                            text: "大小"; font.bold: true; color: "#1f3347"; Layout.preferredWidth: 120
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: window.cycleSort(1)
                            }
                        }
                        Label {
                            text: "修改时间"; font.bold: true; color: "#1f3347"; Layout.preferredWidth: 180
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: window.cycleSort(2)
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

    // 排序与填充
    function applyModeFilter() {
        if (!lastResults || lastResults.length === 0)
            return
        var drivePrefix = window.currentDrive === "全部" ? "" : window.currentDrive + ":\\"
        var filtered = []
        for (var i = 0; i < lastResults.length; i++) {
            var it = lastResults[i]
            var p = "" + it.path
            if (drivePrefix !== "" && p.toUpperCase().indexOf(drivePrefix) !== 0)
                continue 
            var isFolder = ("" + it.isFolder) === "true"
            var ok = false
            switch (window.searchType) {
            case 0:  ok = true; break                                 
            case 1:  ok = isFolder; break                                
            default: ok = !isFolder && ("" + (it.suffix || "")).toLowerCase() === window.searchKeyword.toLowerCase(); break  // 后缀：精确匹配（忽略大小写）
            }
            if (ok)
                filtered.push(it)
        }
        console.log("模式过滤: " + window.searchType + " 盘=" + window.currentDrive
                    + " → " + filtered.length + " / " + lastResults.length)
        requestShow(filtered)
    }

    function requestShow(results) {
        // 新结果
        searchTick++
        beginLoad()
        fileInteract.requestSort(results, window.sortType, searchTick)
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
        if (loadedCount >= displayResults.length)
            return
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

    function reapplySort() {
        if (lastResults.length > 0)
            applyModeFilter()
    }

    function cycleSort(col) {
        var base = (col === 0) ? 0 : (col === 1) ? 2 : 4
        if (window.sortType === base)
            window.sortType = base + 1
        else if (window.sortType === base + 1)
            window.sortType = base
        else
            window.sortType = base
        sortBox.currentIndex = window.sortType
        reapplySort()
    }

    //工具函数 
    function formatSize(size) {
        if (size < 1024) return size + " B"
        if (size < 1024 * 1024) return (size / 1024).toFixed(2) + " KB"
        if (size < 1024 * 1024 * 1024) return (size / 1024 / 1024).toFixed(2) + " MB"
        return (size / 1024 / 1024 / 1024).toFixed(2) + " GB"
    }
}