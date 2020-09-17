/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/

#include "navigatorview.h"
#include "navigatortreemodel.h"
#include "navigatorwidget.h"
#include "qmldesignerconstants.h"
#include "qmldesignericons.h"

#include "nameitemdelegate.h"
#include "iconcheckboxitemdelegate.h"

#include <bindingproperty.h>
#include <designmodecontext.h>
#include <designersettings.h>
#include <nodeproperty.h>
#include <nodelistproperty.h>
#include <variantproperty.h>
#include <qmlitemnode.h>
#include <rewritingexception.h>
#include <nodeinstanceview.h>

#include <coreplugin/editormanager/editormanager.h>
#include <coreplugin/icore.h>

#include <utils/algorithm.h>
#include <utils/icon.h>
#include <utils/utilsicons.h>

#include <QHeaderView>
#include <QTimer>


static inline void setScenePos(const QmlDesigner::ModelNode &modelNode,const QPointF &pos)
{
    if (modelNode.hasParentProperty() && QmlDesigner::QmlItemNode::isValidQmlItemNode(modelNode.parentProperty().parentModelNode())) {
        QmlDesigner::QmlItemNode parentNode = modelNode.parentProperty().parentQmlObjectNode().toQmlItemNode();

        if (!parentNode.modelNode().metaInfo().isLayoutable()) {
            QPointF localPos = parentNode.instanceSceneTransform().inverted().map(pos);
            modelNode.variantProperty("x").setValue(localPos.toPoint().x());
            modelNode.variantProperty("y").setValue(localPos.toPoint().y());
        } else { //Items in Layouts do not have a position
            modelNode.removeProperty("x");
            modelNode.removeProperty("y");
        }
    }
}

static inline void moveNodesUp(const QList<QmlDesigner::ModelNode> &nodes)
{
    for (const auto &node : nodes) {
        if (!node.isRootNode() && node.parentProperty().isNodeListProperty()) {
            int oldIndex = node.parentProperty().indexOf(node);
            int index = oldIndex;
            index--;
            if (index < 0)
                index = node.parentProperty().count() - 1; //wrap around
            if (oldIndex != index)
                node.parentProperty().toNodeListProperty().slide(oldIndex, index);
        }
    }
}

static inline void moveNodesDown(const QList<QmlDesigner::ModelNode> &nodes)
{
    for (const auto &node : nodes) {
        if (!node.isRootNode() && node.parentProperty().isNodeListProperty()) {
            int oldIndex = node.parentProperty().indexOf(node);
            int index = oldIndex;
            index++;
            if (index >= node.parentProperty().count())
                index = 0; //wrap around
            if (oldIndex != index)
                node.parentProperty().toNodeListProperty().slide(oldIndex, index);
        }
    }
}

namespace QmlDesigner {

NavigatorView::NavigatorView(QObject* parent) :
    AbstractView(parent),
    m_blockSelectionChangedSignal(false)
{

}

NavigatorView::~NavigatorView()
{
    if (m_widget && !m_widget->parent())
        delete m_widget.data();
}

bool NavigatorView::hasWidget() const
{
    return true;
}

WidgetInfo NavigatorView::widgetInfo()
{
    if (!m_widget)
        setupWidget();

    return createWidgetInfo(m_widget.data(),
                            new WidgetInfo::ToolBarWidgetDefaultFactory<NavigatorWidget>(m_widget.data()),
                            QStringLiteral("Navigator"),
                            WidgetInfo::LeftPane,
                            0,
                            tr("Navigator"));
}

void NavigatorView::modelAttached(Model *model)
{
    AbstractView::modelAttached(model);

    QTreeView *treeView = treeWidget();

    treeView->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    treeView->header()->resizeSection(1,26);
    treeView->setIndentation(20);

    m_currentModelInterface->setFilter(false);


    QTimer::singleShot(0, this, [this, treeView]() {
        m_currentModelInterface->setFilter(
                    DesignerSettings::getValue(DesignerSettingsKey::NAVIGATOR_SHOW_ONLY_VISIBLE_ITEMS).toBool());

        m_currentModelInterface->setOrder(
                    DesignerSettings::getValue(DesignerSettingsKey::NAVIGATOR_REVERSE_ITEM_ORDER).toBool());

        // Expand everything to begin with to ensure model node to index cache is populated
        treeView->expandAll();

        if (AbstractView::model() && m_expandMap.contains(AbstractView::model()->fileUrl())) {
            const QHash<QString, bool> localExpandMap = m_expandMap[AbstractView::model()->fileUrl()];
            auto it = localExpandMap.constBegin();
            while (it != localExpandMap.constEnd()) {
                const QModelIndex index = indexForModelNode(modelNodeForId(it.key()));
                if (index.isValid())
                    treeWidget()->setExpanded(index, it.value());
                ++it;
            }
        }
    });

#ifdef _LOCK_ITEMS_
    treeView->header()->resizeSection(2,20);
#endif
}

void NavigatorView::modelAboutToBeDetached(Model *model)
{
    m_expandMap.remove(model->fileUrl());

    if (currentModel()) {
        // Store expand state of the navigator tree
        QHash<QString, bool> localExpandMap;
        const ModelNode rootNode = rootModelNode();
        const QModelIndex rootIndex = indexForModelNode(rootNode);

        std::function<void(const QModelIndex &)> gatherExpandedState;
        gatherExpandedState = [&](const QModelIndex &index) {
            if (index.isValid()) {
                const int rowCount = currentModel()->rowCount(index);
                for (int i = 0; i < rowCount; ++i) {
                    const QModelIndex childIndex = currentModel()->index(i, 0, index);
                    const ModelNode node = modelNodeForIndex(childIndex);
                    // Just store collapsed states as everything is expanded by default
                    if (node.isValid() && !treeWidget()->isExpanded(childIndex))
                        localExpandMap.insert(node.id(), false);
                    gatherExpandedState(childIndex);
                }
            }
        };
        gatherExpandedState(rootIndex);
        m_expandMap[model->fileUrl()] = localExpandMap;
    }

    AbstractView::modelAboutToBeDetached(model);
}

void NavigatorView::importsChanged(const QList<Import> &/*addedImports*/, const QList<Import> &/*removedImports*/)
{
    treeWidget()->update();
}

void NavigatorView::bindingPropertiesChanged(const QList<BindingProperty> & propertyList, PropertyChangeFlags /*propertyChange*/)
{
    for (const BindingProperty &bindingProperty : propertyList) {
        /* If a binding property that exports an item using an alias property has
         * changed, we have to update the affected item.
         */

        if (bindingProperty.isAliasExport())
            m_currentModelInterface->notifyDataChanged(modelNodeForId(bindingProperty.expression()));
    }
}

void NavigatorView::customNotification(const AbstractView *view, const QString &identifier,
                                       const QList<ModelNode> &nodeList, const QList<QVariant> &data)
{
    Q_UNUSED(view)
    Q_UNUSED(nodeList)
    Q_UNUSED(data)

    if (identifier == "asset_import_update")
        m_currentModelInterface->notifyIconsChanged();
}

void NavigatorView::handleChangedExport(const ModelNode &modelNode, bool exported)
{
    const ModelNode rootNode = rootModelNode();
    Q_ASSERT(rootNode.isValid());
    const PropertyName modelNodeId = modelNode.id().toUtf8();
    if (rootNode.hasProperty(modelNodeId))
        rootNode.removeProperty(modelNodeId);
    if (exported) {
        executeInTransaction("NavigatorTreeModel:exportItem", [modelNode](){
            QmlObjectNode qmlObjectNode(modelNode);
            qmlObjectNode.ensureAliasExport();
        });
    }
}

bool NavigatorView::isNodeInvisible(const ModelNode &modelNode) const
{
    return QmlVisualNode(modelNode).visibilityOverride();
}

void NavigatorView::disableWidget()
{
    if (m_widget)
        m_widget->disableNavigator();
}

void NavigatorView::enableWidget()
{
    if (m_widget)
        m_widget->enableNavigator();
}

void NavigatorView::modelNodePreviewImageChanged(const ModelNode &node, const QImage &image)
{
    m_treeModel->updateToolTipImage(node, image);
}

ModelNode NavigatorView::modelNodeForIndex(const QModelIndex &modelIndex) const
{
    return modelIndex.model()->data(modelIndex, ModelNodeRole).value<ModelNode>();
}

void NavigatorView::nodeAboutToBeRemoved(const ModelNode & /*removedNode*/)
{
}

void NavigatorView::nodeRemoved(const ModelNode &removedNode,
                                const NodeAbstractProperty & /*parentProperty*/,
                                AbstractView::PropertyChangeFlags /*propertyChange*/)
{
    m_currentModelInterface->notifyModelNodesRemoved({removedNode});
}

void NavigatorView::nodeReparented(const ModelNode &modelNode,
                                   const NodeAbstractProperty & /*newPropertyParent*/,
                                   const NodeAbstractProperty & oldPropertyParent,
                                   AbstractView::PropertyChangeFlags /*propertyChange*/)
{
    if (!oldPropertyParent.isValid())
        m_currentModelInterface->notifyModelNodesInserted({modelNode});
    else
        m_currentModelInterface->notifyModelNodesMoved({modelNode});
    treeWidget()->expand(indexForModelNode(modelNode));

    // make sure selection is in sync again
    QTimer::singleShot(0, this, &NavigatorView::updateItemSelection);
}

void NavigatorView::nodeIdChanged(const ModelNode& modelNode, const QString & /*newId*/, const QString & /*oldId*/)
{
    m_currentModelInterface->notifyDataChanged(modelNode);
}

void NavigatorView::propertiesAboutToBeRemoved(const QList<AbstractProperty>& /*propertyList*/)
{
}

void NavigatorView::propertiesRemoved(const QList<AbstractProperty> &propertyList)
{
    QList<ModelNode> modelNodes;
    for (const AbstractProperty &property : propertyList) {
        if (property.isNodeAbstractProperty()) {
            NodeAbstractProperty nodeAbstractProperty(property.toNodeListProperty());
            modelNodes.append(nodeAbstractProperty.directSubNodes());
        }
    }

    m_currentModelInterface->notifyModelNodesRemoved(modelNodes);
}

void NavigatorView::rootNodeTypeChanged(const QString & /*type*/, int /*majorVersion*/, int /*minorVersion*/)
{
    m_currentModelInterface->notifyDataChanged(rootModelNode());
}

void NavigatorView::nodeTypeChanged(const ModelNode &modelNode, const TypeName &, int , int)
{
    m_currentModelInterface->notifyDataChanged(modelNode);
}

void NavigatorView::auxiliaryDataChanged(const ModelNode &modelNode,
                                         const PropertyName & /*name*/,
                                         const QVariant & /*data*/)
{
    m_currentModelInterface->notifyDataChanged(modelNode);
}

void NavigatorView::instanceErrorChanged(const QVector<ModelNode> &errorNodeList)
{
    for (const ModelNode &modelNode : errorNodeList)
        m_currentModelInterface->notifyDataChanged(modelNode);
}

void NavigatorView::nodeOrderChanged(const NodeListProperty & listProperty,
                                     const ModelNode & /*node*/,
                                     int /*oldIndex*/)
{
    m_currentModelInterface->notifyModelNodesMoved(listProperty.directSubNodes());

    // make sure selection is in sync again
    QTimer::singleShot(0, this, &NavigatorView::updateItemSelection);
}

void NavigatorView::changeToComponent(const QModelIndex &index)
{
    if (index.isValid() && currentModel()->data(index, Qt::UserRole).isValid()) {
        const ModelNode doubleClickNode = modelNodeForIndex(index);
        if (doubleClickNode.metaInfo().isFileComponent())
            Core::EditorManager::openEditor(doubleClickNode.metaInfo().componentFileName(),
                                            Utils::Id(), Core::EditorManager::DoNotMakeVisible);
    }
}

QModelIndex NavigatorView::indexForModelNode(const ModelNode &modelNode) const
{
    return m_currentModelInterface->indexForModelNode(modelNode);
}

QAbstractItemModel *NavigatorView::currentModel() const
{
    return treeWidget()->model();
}

void NavigatorView::leftButtonClicked()
{
    if (selectedModelNodes().count() > 1)
        return; //Semantics are unclear for multi selection.

    bool blocked = blockSelectionChangedSignal(true);

    for (const ModelNode &node : selectedModelNodes()) {
        if (!node.isRootNode() && !node.parentProperty().parentModelNode().isRootNode()) {
            if (QmlItemNode::isValidQmlItemNode(node)) {
                QPointF scenePos = QmlItemNode(node).instanceScenePosition();
                reparentAndCatch(node.parentProperty().parentProperty(), node);
                if (!scenePos.isNull())
                    setScenePos(node, scenePos);
            } else {
                reparentAndCatch(node.parentProperty().parentProperty(), node);
            }
        }
    }

    updateItemSelection();
    blockSelectionChangedSignal(blocked);
}

void NavigatorView::rightButtonClicked()
{
    if (selectedModelNodes().count() > 1)
        return; //Semantics are unclear for multi selection.

    bool blocked = blockSelectionChangedSignal(true);
    bool reverse = DesignerSettings::getValue(DesignerSettingsKey::NAVIGATOR_REVERSE_ITEM_ORDER).toBool();

    for (const ModelNode &node : selectedModelNodes()) {
        if (!node.isRootNode() && node.parentProperty().isNodeListProperty() && node.parentProperty().count() > 1) {
            int index = node.parentProperty().indexOf(node);

            bool indexOk = false;

            if (reverse) {
                index++;
                indexOk = (index < node.parentProperty().count());
            } else {
                index--;
                indexOk = (index >= 0);
            }

            if (indexOk) { //for the first node the semantics are not clear enough. Wrapping would be irritating.
                ModelNode newParent = node.parentProperty().toNodeListProperty().at(index);

                if (QmlItemNode::isValidQmlItemNode(node)
                        && QmlItemNode::isValidQmlItemNode(newParent)
                        && !newParent.metaInfo().defaultPropertyIsComponent()) {
                    QPointF scenePos = QmlItemNode(node).instanceScenePosition();
                    reparentAndCatch(newParent.nodeAbstractProperty(newParent.metaInfo().defaultPropertyName()), node);
                    if (!scenePos.isNull())
                        setScenePos(node, scenePos);
                } else {
                    if (newParent.metaInfo().isValid() && !newParent.metaInfo().defaultPropertyIsComponent())
                        reparentAndCatch(newParent.nodeAbstractProperty(newParent.metaInfo().defaultPropertyName()), node);
                }
            }
        }
    }
    updateItemSelection();
    blockSelectionChangedSignal(blocked);
}

void NavigatorView::upButtonClicked()
{
    bool blocked = blockSelectionChangedSignal(true);
    bool reverse = DesignerSettings::getValue(DesignerSettingsKey::NAVIGATOR_REVERSE_ITEM_ORDER).toBool();

    if (reverse)
        moveNodesDown(selectedModelNodes());
    else
        moveNodesUp(selectedModelNodes());

    updateItemSelection();
    blockSelectionChangedSignal(blocked);
}

void NavigatorView::downButtonClicked()
{
    bool blocked = blockSelectionChangedSignal(true);
    bool reverse = DesignerSettings::getValue(DesignerSettingsKey::NAVIGATOR_REVERSE_ITEM_ORDER).toBool();

    if (reverse)
        moveNodesUp(selectedModelNodes());
    else
        moveNodesDown(selectedModelNodes());

    updateItemSelection();
    blockSelectionChangedSignal(blocked);
}

void NavigatorView::filterToggled(bool flag)
{
    m_currentModelInterface->setFilter(flag);
    treeWidget()->expandAll();
    DesignerSettings::setValue(DesignerSettingsKey::NAVIGATOR_SHOW_ONLY_VISIBLE_ITEMS, flag);
}

void NavigatorView::reverseOrderToggled(bool flag)
{
    m_currentModelInterface->setOrder(flag);
    treeWidget()->expandAll();
    DesignerSettings::setValue(DesignerSettingsKey::NAVIGATOR_REVERSE_ITEM_ORDER, flag);
}

void NavigatorView::changeSelection(const QItemSelection & /*newSelection*/, const QItemSelection &/*deselected*/)
{
    if (m_blockSelectionChangedSignal)
        return;

    QSet<ModelNode> nodeSet;

    for (const QModelIndex &index : treeWidget()->selectionModel()->selectedIndexes()) {

        const ModelNode modelNode = modelNodeForIndex(index);
        if (modelNode.isValid())
            nodeSet.insert(modelNode);
    }

    bool blocked = blockSelectionChangedSignal(true);
    setSelectedModelNodes(Utils::toList(nodeSet));
    blockSelectionChangedSignal(blocked);
}

void NavigatorView::selectedNodesChanged(const QList<ModelNode> &/*selectedNodeList*/, const QList<ModelNode> &/*lastSelectedNodeList*/)
{
    // Update selection asynchronously to ensure NavigatorTreeModel's index cache is up to date
    QTimer::singleShot(0, this, &NavigatorView::updateItemSelection);
}

void NavigatorView::updateItemSelection()
{
    if (!isAttached())
        return;

    QItemSelection itemSelection;
    for (const ModelNode &node : selectedModelNodes()) {
        const QModelIndex index = indexForModelNode(node);

        if (index.isValid()) {
            const QModelIndex beginIndex(currentModel()->index(index.row(), 0, index.parent()));
            const QModelIndex endIndex(currentModel()->index(index.row(), currentModel()->columnCount(index.parent()) - 1, index.parent()));
            if (beginIndex.isValid() && endIndex.isValid())
                itemSelection.select(beginIndex, endIndex);
        } else {
            // if the node index is invalid expand ancestors manually if they are valid.
            ModelNode parentNode = node;
            while (parentNode.hasParentProperty()) {
                parentNode = parentNode.parentProperty().parentQmlObjectNode();
                QModelIndex parentIndex = indexForModelNode(parentNode);
                if (parentIndex.isValid())
                    treeWidget()->expand(parentIndex);
                else
                    break;
            }
         }
    }

    bool blocked = blockSelectionChangedSignal(true);
    treeWidget()->selectionModel()->select(itemSelection, QItemSelectionModel::ClearAndSelect);
    blockSelectionChangedSignal(blocked);

    if (!selectedModelNodes().isEmpty())
        treeWidget()->scrollTo(indexForModelNode(selectedModelNodes().constFirst()));

    // make sure selected nodes are visible
    for (const QModelIndex &selectedIndex : itemSelection.indexes()) {
        if (selectedIndex.column() == 0)
            expandAncestors(selectedIndex);
    }
}

QTreeView *NavigatorView::treeWidget() const
{
    if (m_widget)
        return m_widget->treeView();
    return nullptr;
}

NavigatorTreeModel *NavigatorView::treeModel()
{
    return m_treeModel.data();
}

// along the lines of QObject::blockSignals
bool NavigatorView::blockSelectionChangedSignal(bool block)
{
    bool oldValue = m_blockSelectionChangedSignal;
    m_blockSelectionChangedSignal = block;
    return oldValue;
}

void NavigatorView::expandAncestors(const QModelIndex &index)
{
    QModelIndex currentIndex = index.parent();
    while (currentIndex.isValid()) {
        if (!treeWidget()->isExpanded(currentIndex))
            treeWidget()->expand(currentIndex);
        currentIndex = currentIndex.parent();
    }
}

void NavigatorView::reparentAndCatch(NodeAbstractProperty property, const ModelNode &modelNode)
{
    try {
        property.reparentHere(modelNode);
    }  catch (Exception &exception) {
        exception.showException();
    }
}

void NavigatorView::setupWidget()
{
    m_widget = new NavigatorWidget(this);
    m_treeModel = new NavigatorTreeModel(this);

#ifndef QMLDESIGNER_TEST
    auto navigatorContext = new Internal::NavigatorContext(m_widget.data());
    Core::ICore::addContextObject(navigatorContext);
#endif

    m_treeModel->setView(this);
    m_widget->setTreeModel(m_treeModel.data());
    m_currentModelInterface = m_treeModel;

    connect(treeWidget()->selectionModel(), &QItemSelectionModel::selectionChanged, this, &NavigatorView::changeSelection);

    connect(m_widget.data(), &NavigatorWidget::leftButtonClicked, this, &NavigatorView::leftButtonClicked);
    connect(m_widget.data(), &NavigatorWidget::rightButtonClicked, this, &NavigatorView::rightButtonClicked);
    connect(m_widget.data(), &NavigatorWidget::downButtonClicked, this, &NavigatorView::downButtonClicked);
    connect(m_widget.data(), &NavigatorWidget::upButtonClicked, this, &NavigatorView::upButtonClicked);
    connect(m_widget.data(), &NavigatorWidget::filterToggled, this, &NavigatorView::filterToggled);
    connect(m_widget.data(), &NavigatorWidget::reverseOrderToggled, this, &NavigatorView::reverseOrderToggled);

#ifndef QMLDESIGNER_TEST
    auto idDelegate = new NameItemDelegate(this);
    IconCheckboxItemDelegate *showDelegate =
            new IconCheckboxItemDelegate(this,
                                         Utils::Icons::EYE_OPEN_TOOLBAR.icon(),
                                         Utils::Icons::EYE_CLOSED_TOOLBAR.icon());

    IconCheckboxItemDelegate *exportDelegate =
            new IconCheckboxItemDelegate(this,
                                         Icons::EXPORT_CHECKED.icon(),
                                         Icons::EXPORT_UNCHECKED.icon());

#ifdef _LOCK_ITEMS_
    IconCheckboxItemDelegate *lockDelegate =
            new IconCheckboxItemDelegate(this,
                                         Utils::Icons::LOCKED_TOOLBAR.icon(),
                                         Utils::Icons::UNLOCKED_TOOLBAR.icon());
#endif


    treeWidget()->setItemDelegateForColumn(0, idDelegate);
#ifdef _LOCK_ITEMS_
    treeWidget()->setItemDelegateForColumn(1,lockDelegate);
    treeWidget()->setItemDelegateForColumn(2,showDelegate);
#else
    treeWidget()->setItemDelegateForColumn(1, exportDelegate);
    treeWidget()->setItemDelegateForColumn(2, showDelegate);
#endif

#endif //QMLDESIGNER_TEST
}

} // namespace QmlDesigner
