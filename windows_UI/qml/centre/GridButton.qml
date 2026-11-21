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
        hoverEnabled: true
        text: L10n.tr("grid.title")
        checkable: true
        checked: control.editor.gridVisible
        onToggled: control.editor.gridVisible = checked
        background: Rectangle {
            color: "transparent"; radius: 4
            border.color: toggle.visualFocus ? Theme.accent : "transparent"
        }
        contentItem: Text {
            text: Icons.gridView
            font.family: Theme.iconFamily
            font.pixelSize: Theme.toolGlyph
            color: toggle.checked ? Theme.accent : Theme.text
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
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
        hoverEnabled: true
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
