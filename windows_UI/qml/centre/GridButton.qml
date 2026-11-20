import QtQuick
import QtQuick.Controls

Rectangle {
    id: control
    required property EditorState editor

    implicitWidth: toggle.width + arrow.width
    implicitHeight: Theme.toolButton
    radius: 4
    color: toggle.hovered || arrow.hovered || menu.visible ? Theme.hover : "transparent"

    AbstractButton {
        id: toggle
        objectName: "gridToggleButton"
        width: Theme.toolButton
        height: parent.height
        text: L10n.tr("grid.title")
        checkable: true
        checked: control.editor.gridVisible
        onToggled: control.editor.gridVisible = checked
        background: Rectangle {
            color: "transparent"; radius: 4
            border.color: toggle.visualFocus ? Theme.accent : "transparent"
        }
        contentItem: Item {
            Rectangle {
                id: glyph
                anchors.centerIn: parent
                width: 16; height: 16; radius: 2
                color: "transparent"
                border.color: toggle.checked ? Theme.accent : Theme.text
                Repeater {
                    model: 4
                    Rectangle {
                        required property int index
                        readonly property bool vertical: index < 2
                        x: vertical ? 5 + index * 5 : 0
                        y: vertical ? 0 : 5 + (index - 2) * 5
                        width: vertical ? 1 : glyph.width
                        height: vertical ? glyph.height : 1
                        color: glyph.border.color
                    }
                }
            }
        }
        ToolTip.visible: hovered
        ToolTip.text: text
        ToolTip.delay: 500
    }

    AbstractButton {
        id: arrow
        objectName: "gridStyleButton"
        anchors.left: toggle.right
        width: 18
        height: parent.height
        text: L10n.tr("grid.style")
        onClicked: menu.visible ? menu.close() : menu.popup(control, control.width - menu.width, control.height + 2)
        background: Rectangle {
            color: "transparent"; radius: 4
            border.color: arrow.visualFocus ? Theme.accent : "transparent"
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 1; height: 12
                color: Theme.rule
            }
        }
        contentItem: Text {
            text: Icons.chevronDown
            font.family: Theme.iconFamily
            font.pixelSize: 9
            color: Theme.text
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        ToolTip.visible: hovered && !menu.visible
        ToolTip.text: text
        ToolTip.delay: 500
    }

    component StyleOption: RailMenuItem {
        required property int divisions
        objectName: "gridStyle" + divisions
        marked: control.editor.gridStyle === divisions
        onTriggered: control.editor.selectGridStyle(divisions)
    }
    RailMenu {
        id: menu
        objectName: "gridStyleMenu"
        StyleOption { divisions: 2; text: L10n.tr("grid.cross") }
        StyleOption { divisions: 3; text: L10n.tr("grid.thirds") }
        StyleOption { divisions: 10; text: L10n.tr("grid.dense") }
    }
}
