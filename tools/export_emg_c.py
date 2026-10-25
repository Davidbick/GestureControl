from pathlib import Path

import numpy as np
from sklearn.tree import _tree


def export_emg_tree(model, feature_names, output_dir):
    """
    Export a fitted scikit-learn DecisionTreeClassifier
    into standalone C code.

    Class encoding:
        0 = REST
        1 = FLEX
        2 = EXTEND
        3 = TENSE
    """
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    if not hasattr(model, "tree_"):
        raise ValueError("Expected a fitted DecisionTreeClassifier")

    tree = model.tree_
    names = list(feature_names)
    if model.n_features_in_ != len(names) or len(names) != 8:
        raise ValueError("Expected exactly eight EMG features")

    classes = [str(c) for c in model.classes_]
    expected = {"REST", "FLEX", "EXTEND", "TENSE"}

    if set(classes) != expected:
        raise ValueError(f"Unexpected classes: {classes}")

    header = """#ifndef EMG_MODEL_H
#define EMG_MODEL_H

typedef enum {
    EMG_REST = 0,
    EMG_FLEX = 1,
    EMG_EXTEND = 2,
    EMG_TENSE = 3,
    EMG_UNKNOWN = 4
} emg_class_t;

#define EMG_FEATURE_COUNT %d

emg_class_t emg_model_predict(
    const float features[EMG_FEATURE_COUNT],
    const float baseline_mv[2]);

#endif
""" % len(names)

    lines = [
        '#include "emg_model.h"',
        "",
        "emg_class_t emg_model_predict(",
        "    const float features[EMG_FEATURE_COUNT],",
        "    const float baseline_mv[2])",
        "{",
        "    // Trained tree uses raw eight-feature input.",
        "    (void)baseline_mv;",
    ]

    class_map = {
        "REST": "EMG_REST",
        "FLEX": "EMG_FLEX",
        "EXTEND": "EMG_EXTEND",
        "TENSE": "EMG_TENSE",
    }

    def emit(node, indent):
        prefix = "    " * indent

        if tree.feature[node] == _tree.TREE_UNDEFINED:
            class_index = int(np.argmax(tree.value[node][0]))
            label = classes[class_index]
            lines.append(
                f"{prefix}return {class_map[label]};"
            )
            return

        feature = int(tree.feature[node])
        threshold = float(tree.threshold[node])

        lines.append(
            f"{prefix}if (features[{feature}] <= "
            f"{threshold:.9e}f) {{"
        )

        emit(tree.children_left[node], indent + 1)

        lines.append(f"{prefix}}} else {{")
        emit(tree.children_right[node], indent + 1)
        lines.append(f"{prefix}}}")

    emit(0, 1)
    lines.append("}")
    lines.append("")

    (output_dir / "emg_model.h").write_text(header)
    (output_dir / "emg_model.c").write_text(
        "\n".join(lines)
    )

    print("Export complete")
    print("Feature order:")
    for index, name in enumerate(names):
        print(f"  features[{index}] = {name}")
    print("Tree nodes:", tree.node_count)
    print("Tree depth:", tree.max_depth)
    print("Output:", output_dir)
