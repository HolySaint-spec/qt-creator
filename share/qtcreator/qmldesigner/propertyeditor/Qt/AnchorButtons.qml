import Qt 4.6
import Bauhaus 1.0


QWidget {
    id: anchorButtons
    fixedHeight: 32

    function isBorderAnchored() {
        return anchorBackend.leftAnchored || anchorBackend.topAnchored || anchorBackend.rightAnchored || anchorBackend.bottomAnchored;
    }

    function fill() {
        anchorBackend.fill();
    }

    function breakLayout() {
        anchorBackend.resetLayout()
    }

    QPushButton {

        checkable: true
        fixedWidth: 31
        fixedHeight: 28
        styleSheetFile: "anchortop.css";

        checked: anchorBackend.topAnchored;
        onToggled: {
            if (checked) {
                anchorBackend.verticalCentered = false;
                anchorBackend.topAnchored = true;
            } else {
                anchorBackend.topAnchored = false;
            }
        }

    }

    QPushButton {

        x: 31
        checkable: true
        fixedWidth: 30
        fixedHeight: 28

        styleSheetFile: "anchorbottom.css";

        checked: anchorBackend.bottomAnchored;
        onToggled: {
            if (checked) {
                anchorBackend.verticalCentered = false;
                anchorBackend.bottomAnchored = true;
            } else {
                anchorBackend.bottomAnchored = false;
            }
        }

    }
    QPushButton {

        x: 61
        checkable: true
        fixedWidth: 30
        fixedHeight: 28

        styleSheetFile: "anchorleft.css";

        checked: anchorBackend.leftAnchored;
        onToggled: {
            if (checked) {
                    anchorBackend.horizontalCentered = false;
                    anchorBackend.leftAnchored = true;
                } else {
                    anchorBackend.leftAnchored = false;
                }
        }
    }

    QPushButton {

        x: 91
        checkable: true
        fixedWidth: 30
        fixedHeight: 28

        styleSheetFile: "anchorright.css";

        checked: anchorBackend.rightAnchored;
        onToggled: {
            if (checked) {
                    anchorBackend.horizontalCentered = false;
                    anchorBackend.rightAnchored = true;
                } else {
                    anchorBackend.rightAnchored = false;
                }
        }
    }

    QPushButton {
        x: 121
        checkable: true
        fixedWidth: 19
        fixedHeight: 28

        styleSheetFile: "anchorspacer.css";


    }

    QPushButton {
        x: 140
        fixedWidth: 30
        fixedHeight: 28

        styleSheetFile: "anchorfill.css";

        onReleased: fill();
    }

    QPushButton {
        x: 170
        checkable: true
        fixedWidth: 19
        fixedHeight: 28

        styleSheetFile: "anchorspacer.css";

    }

    QPushButton {
        x: 189
        checkable: true
        fixedWidth: 30
        fixedHeight: 28

		styleSheetFile: "anchorvertical.css";

        checked: anchorBackend.verticalCentered;
        onToggled: {
            if (checked) {
                    anchorBackend.leftAnchored = false;
                    anchorBackend.rightAnchored = false;
                    anchorBackend.verticalCentered = true;
            } else {
                    anchorBackend.verticalCentered = false;
            }
        }
    }

    QPushButton {
        x: 219
        checkable: true
        fixedWidth: 31
        fixedHeight: 28
        
		styleSheetFile: "anchorhorizontal.css";

        checked: anchorBackend.horizontalCentered;
        onToggled: {
            if (checked) {
                    anchorBackend.topAnchored = false;
                    anchorBackend.bottomAnchored = false;
                    anchorBackend.horizontalCentered = true;
            } else {
                    anchorBackend.horizontalCentered = false;
            }
        }
    }
}

