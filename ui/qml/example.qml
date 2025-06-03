import QtQuick
import QtQuick.Controls

Rectangle {
    width: 400
    height: 300
    color: "#f0f0f0"

    Column {
        anchors.centerIn: parent
        spacing: 20

        // 使用纯 Rectangle + MouseArea 模拟按钮（兼容性最强）
        Rectangle {
            id: customButton
            width: 120
            height: 40
            radius: 5
            color: mouseArea.containsPress ? "#388E3C" : "#4CAF50"
            
            Text {
                text: "QML按钮"
                anchors.centerIn: parent
                color: "white"
                font.pixelSize: 14
            }

            MouseArea {
                id: mouseArea
                anchors.fill: parent
                onClicked: label.text = "按钮被点击！"
            }
        }

        Text {
            id: label
            text: "QML界面示例"
            font.pixelSize: 24
            color: "#333"
        }

        Rectangle {
            width: 200
            height: 50
            radius: 10
            color: "#2196F3"
            Text {
                anchors.centerIn: parent
                text: "自定义组件"
                color: "white"
            }
        }
    }
}