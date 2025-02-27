import QtQuick 2.15
import QtQuick.Controls 2.15

Rectangle {
    width: 400
    height: 300
    color: "#f0f0f0"

    Column {
        anchors.centerIn: parent
        spacing: 20

        Button {
            text: "QML按钮"
            background: Rectangle {
                color: "#4CAF50"
                radius: 5
            }
            onClicked: {
                label.text = "按钮被点击！"
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