/**************************************************************************
**
** This file is part of Qt Creator
**
** Copyright (c) 2010 Nokia Corporation and/or its subsidiary(-ies).
**
** Contact: Nokia Corporation (qt-info@nokia.com)
**
** Commercial Usage
**
** Licensees holding valid Qt Commercial licenses may use this file in
** accordance with the Qt Commercial License Agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Nokia.
**
** GNU Lesser General Public License Usage
**
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** If you are unsure which license is appropriate for your use, please
** contact the sales department at http://qt.nokia.com/contact.
**
**************************************************************************/

#include "qmljseditor.h"
#include "qmljseditorconstants.h"
#include "qmljshighlighter.h"
#include "qmljseditorplugin.h"
#include "qmljsmodelmanager.h"
#include "qmloutlinemodel.h"

#include <qmljs/qmljsindenter.h>
#include <qmljs/qmljsbind.h>
#include <qmljs/qmljscheck.h>
#include <qmljs/qmljsdocument.h>
#include <qmljs/qmljsicontextpane.h>
#include <qmljs/qmljslookupcontext.h>
#include <qmljs/parser/qmljsastvisitor_p.h>
#include <qmljs/parser/qmljsast_p.h>
#include <qmljs/parser/qmljsengine_p.h>

#include <coreplugin/actionmanager/actionmanager.h>
#include <coreplugin/actionmanager/actioncontainer.h>
#include <coreplugin/actionmanager/command.h>
#include <coreplugin/editormanager/editormanager.h>
#include <coreplugin/icore.h>
#include <coreplugin/coreconstants.h>
#include <coreplugin/modemanager.h>
#include <coreplugin/designmode.h>
#include <coreplugin/mimedatabase.h>
#include <extensionsystem/pluginmanager.h>
#include <texteditor/basetextdocument.h>
#include <texteditor/fontsettings.h>
#include <texteditor/textblockiterator.h>
#include <texteditor/texteditorconstants.h>
#include <texteditor/texteditorsettings.h>
#include <texteditor/syntaxhighlighter.h>
#include <qmldesigner/qmldesignerconstants.h>
#include <utils/changeset.h>
#include <utils/uncommentselection.h>

#include <QtCore/QFileInfo>
#include <QtCore/QTimer>

#include <QtGui/QMenu>
#include <QtGui/QComboBox>
#include <QtGui/QHeaderView>
#include <QtGui/QInputDialog>
#include <QtGui/QMainWindow>
#include <QtGui/QToolBar>
#include <QtGui/QTreeView>

enum {
    UPDATE_DOCUMENT_DEFAULT_INTERVAL = 50,
    UPDATE_USES_DEFAULT_INTERVAL = 150,
    UPDATE_OUTLINE_INTERVAL = 150 // msecs after new semantic info has been arrived / cursor has moved
};

using namespace QmlJS;
using namespace QmlJS::AST;
using namespace QmlJSEditor::Internal;

static int blockStartState(const QTextBlock &block)
{
    int state = block.userState();

    if (state == -1)
        return 0;
    else
        return state & 0xff;
}

static bool shouldInsertMatchingText(QChar lookAhead)
{
    switch (lookAhead.unicode()) {
    case '{': case '}':
    case ']': case ')':
    case ';': case ',':
    case '"': case '\'':
        return true;

    default:
        if (lookAhead.isSpace())
            return true;

        return false;
    } // switch
}

static bool shouldInsertMatchingText(const QTextCursor &tc)
{
    QTextDocument *doc = tc.document();
    return shouldInsertMatchingText(doc->characterAt(tc.selectionEnd()));
}

namespace {

class FindIdDeclarations: protected Visitor
{
public:
    typedef QHash<QString, QList<AST::SourceLocation> > Result;

    Result operator()(Document::Ptr doc)
    {
        _ids.clear();
        _maybeIds.clear();
        if (doc && doc->qmlProgram())
            doc->qmlProgram()->accept(this);
        return _ids;
    }

protected:
    QString asString(AST::UiQualifiedId *id)
    {
        QString text;
        for (; id; id = id->next) {
            if (id->name)
                text += id->name->asString();
            else
                text += QLatin1Char('?');

            if (id->next)
                text += QLatin1Char('.');
        }

        return text;
    }

    void accept(AST::Node *node)
    { AST::Node::acceptChild(node, this); }

    using Visitor::visit;
    using Visitor::endVisit;

    virtual bool visit(AST::UiScriptBinding *node)
    {
        if (asString(node->qualifiedId) == QLatin1String("id")) {
            if (AST::ExpressionStatement *stmt = AST::cast<AST::ExpressionStatement*>(node->statement)) {
                if (AST::IdentifierExpression *idExpr = AST::cast<AST::IdentifierExpression *>(stmt->expression)) {
                    if (idExpr->name) {
                        const QString id = idExpr->name->asString();
                        QList<AST::SourceLocation> *locs = &_ids[id];
                        locs->append(idExpr->firstSourceLocation());
                        locs->append(_maybeIds.value(id));
                        _maybeIds.remove(id);
                        return false;
                    }
                }
            }
        }

        accept(node->statement);

        return false;
    }

    virtual bool visit(AST::IdentifierExpression *node)
    {
        if (node->name) {
            const QString name = node->name->asString();

            if (_ids.contains(name))
                _ids[name].append(node->identifierToken);
            else
                _maybeIds[name].append(node->identifierToken);
        }
        return false;
    }

private:
    Result _ids;
    Result _maybeIds;
};

class FindDeclarations: protected Visitor
{
    QList<Declaration> _declarations;
    int _depth;

public:
    QList<Declaration> operator()(AST::Node *node)
    {
        _depth = -1;
        _declarations.clear();
        accept(node);
        return _declarations;
    }

protected:
    using Visitor::visit;
    using Visitor::endVisit;

    QString asString(AST::UiQualifiedId *id)
    {
        QString text;
        for (; id; id = id->next) {
            if (id->name)
                text += id->name->asString();
            else
                text += QLatin1Char('?');

            if (id->next)
                text += QLatin1Char('.');
        }

        return text;
    }

    void accept(AST::Node *node)
    { AST::Node::acceptChild(node, this); }

    void init(Declaration *decl, AST::UiObjectMember *member)
    {
        const SourceLocation first = member->firstSourceLocation();
        const SourceLocation last = member->lastSourceLocation();
        decl->startLine = first.startLine;
        decl->startColumn = first.startColumn;
        decl->endLine = last.startLine;
        decl->endColumn = last.startColumn + last.length;
    }

    void init(Declaration *decl, AST::ExpressionNode *expressionNode)
    {
        const SourceLocation first = expressionNode->firstSourceLocation();
        const SourceLocation last = expressionNode->lastSourceLocation();
        decl->startLine = first.startLine;
        decl->startColumn = first.startColumn;
        decl->endLine = last.startLine;
        decl->endColumn = last.startColumn + last.length;
    }

    virtual bool visit(AST::UiObjectDefinition *node)
    {
        ++_depth;

        Declaration decl;
        init(&decl, node);

        decl.text.fill(QLatin1Char(' '), _depth);
        if (node->qualifiedTypeNameId)
            decl.text.append(asString(node->qualifiedTypeNameId));
        else
            decl.text.append(QLatin1Char('?'));

        _declarations.append(decl);

        return true; // search for more bindings
    }

    virtual void endVisit(AST::UiObjectDefinition *)
    {
        --_depth;
    }

    virtual bool visit(AST::UiObjectBinding *node)
    {
        ++_depth;

        Declaration decl;
        init(&decl, node);

        decl.text.fill(QLatin1Char(' '), _depth);

        decl.text.append(asString(node->qualifiedId));
        decl.text.append(QLatin1String(": "));

        if (node->qualifiedTypeNameId)
            decl.text.append(asString(node->qualifiedTypeNameId));
        else
            decl.text.append(QLatin1Char('?'));

        _declarations.append(decl);

        return true; // search for more bindings
    }

    virtual void endVisit(AST::UiObjectBinding *)
    {
        --_depth;
    }

    virtual bool visit(AST::UiScriptBinding *)
    {
        ++_depth;

#if 0 // ### ignore script bindings for now.
        Declaration decl;
        init(&decl, node);

        decl.text.fill(QLatin1Char(' '), _depth);
        decl.text.append(asString(node->qualifiedId));

        _declarations.append(decl);
#endif

        return false; // more more bindings in this subtree.
    }

    virtual void endVisit(AST::UiScriptBinding *)
    {
        --_depth;
    }

    virtual bool visit(AST::FunctionExpression *)
    {
        return false;
    }

    virtual bool visit(AST::FunctionDeclaration *ast)
    {
        if (! ast->name)
            return false;

        Declaration decl;
        init(&decl, ast);

        decl.text.fill(QLatin1Char(' '), _depth);
        decl.text += ast->name->asString();

        decl.text += QLatin1Char('(');
        for (FormalParameterList *it = ast->formals; it; it = it->next) {
            if (it->name)
                decl.text += it->name->asString();

            if (it->next)
                decl.text += QLatin1String(", ");
        }

        decl.text += QLatin1Char(')');

        _declarations.append(decl);

        return false;
    }

    virtual bool visit(AST::VariableDeclaration *ast)
    {
        if (! ast->name)
            return false;

        Declaration decl;
        decl.text.fill(QLatin1Char(' '), _depth);
        decl.text += ast->name->asString();

        const SourceLocation first = ast->identifierToken;
        decl.startLine = first.startLine;
        decl.startColumn = first.startColumn;
        decl.endLine = first.startLine;
        decl.endColumn = first.startColumn + first.length;

        _declarations.append(decl);

        return false;
    }
};

class CreateRanges: protected AST::Visitor
{
    QTextDocument *_textDocument;
    QList<Range> _ranges;

public:
    QList<Range> operator()(QTextDocument *textDocument, Document::Ptr doc)
    {
        _textDocument = textDocument;
        _ranges.clear();
        if (doc && doc->ast() != 0)
            doc->ast()->accept(this);
        return _ranges;
    }

protected:
    using AST::Visitor::visit;

    virtual bool visit(AST::UiObjectBinding *ast)
    {
        if (ast->initializer)
            _ranges.append(createRange(ast, ast->initializer));
        return true;
    }

    virtual bool visit(AST::UiObjectDefinition *ast)
    {
        if (ast->initializer)
            _ranges.append(createRange(ast, ast->initializer));
        return true;
    }

    virtual bool visit(AST::FunctionExpression *ast)
    {
        _ranges.append(createRange(ast));
        return true;
    }

    virtual bool visit(AST::FunctionDeclaration *ast)
    {
        _ranges.append(createRange(ast));
        return true;
    }

    Range createRange(AST::UiObjectMember *member, AST::UiObjectInitializer *ast)
    {
        Range range;

        range.ast = member;

        range.begin = QTextCursor(_textDocument);
        range.begin.setPosition(member->firstSourceLocation().begin());

        range.end = QTextCursor(_textDocument);
        range.end.setPosition(ast->rbraceToken.end());
        return range;
    }

    Range createRange(AST::FunctionExpression *ast)
    {
        Range range;

        range.ast = ast;

        range.begin = QTextCursor(_textDocument);
        range.begin.setPosition(ast->lbraceToken.begin());

        range.end = QTextCursor(_textDocument);
        range.end.setPosition(ast->rbraceToken.end());

        return range;
    }

};


class CollectASTNodes: protected AST::Visitor
{
public:
    QList<AST::UiQualifiedId *> qualifiedIds;
    QList<AST::IdentifierExpression *> identifiers;
    QList<AST::FieldMemberExpression *> fieldMembers;

    void accept(AST::Node *node)
    {
        if (node)
            node->accept(this);
    }

protected:
    using AST::Visitor::visit;

    virtual bool visit(AST::UiQualifiedId *ast)
    {
        qualifiedIds.append(ast);
        return false;
    }

    virtual bool visit(AST::IdentifierExpression *ast)
    {
        identifiers.append(ast);
        return false;
    }

    virtual bool visit(AST::FieldMemberExpression *ast)
    {
        fieldMembers.append(ast);
        return true;
    }
};

} // end of anonymous namespace


AST::Node *SemanticInfo::declaringMember(int cursorPosition) const
{
    AST::Node *declaringMember = 0;

    for (int i = ranges.size() - 1; i != -1; --i) {
        const Range &range = ranges.at(i);

        if (range.begin.isNull() || range.end.isNull()) {
            continue;
        } else if (cursorPosition >= range.begin.position() && cursorPosition <= range.end.position()) {
            declaringMember = range.ast;
            break;
        }
    }

    return declaringMember;
}

QmlJS::AST::Node *SemanticInfo::declaringMemberNoProperties(int cursorPosition) const
{
   AST::Node *node = declaringMember(cursorPosition);

   if (UiObjectDefinition *objectDefinition = cast<UiObjectDefinition*>(node)) {
       QString name = objectDefinition->qualifiedTypeNameId->name->asString();
       if (!name.isNull() && name.at(0).isLower()) {
           QList<AST::Node *> path = astPath(cursorPosition);
           if (path.size() > 1)
               return path.at(path.size() - 2);
       } else if (name.contains("GradientStop")) {
           QList<AST::Node *> path = astPath(cursorPosition);
           if (path.size() > 2)
               return path.at(path.size() - 3);
       }
   } else if (UiObjectBinding *objectBinding = cast<UiObjectBinding*>(node)) {
       QString name = objectBinding->qualifiedTypeNameId->name->asString();
       if (name.contains("Gradient")) {
           QList<AST::Node *> path = astPath(cursorPosition);
           if (path.size() > 1)
               return path.at(path.size() - 2);
       }
   }

   return node;
}

QList<AST::Node *> SemanticInfo::astPath(int cursorPosition) const
{
    QList<AST::Node *> path;

    foreach (const Range &range, ranges) {
        if (range.begin.isNull() || range.end.isNull()) {
            continue;
        } else if (cursorPosition >= range.begin.position() && cursorPosition <= range.end.position()) {
            path += range.ast;
        }
    }

    return path;
}

static bool importContainsCursor(UiImport *importAst, unsigned cursorPosition)
{
    return cursorPosition >= importAst->firstSourceLocation().begin()
           && cursorPosition <= importAst->lastSourceLocation().end();
}

AST::Node *SemanticInfo::nodeUnderCursor(int pos) const
{
    if (! document)
        return 0;

    const unsigned cursorPosition = pos;

    foreach (const Bind::ImportInfo &import, document->bind()->fileImports()) {
        if (importContainsCursor(import.ast, cursorPosition))
            return import.ast;
    }
    foreach (const Bind::ImportInfo &import, document->bind()->directoryImports()) {
        if (importContainsCursor(import.ast, cursorPosition))
            return import.ast;
    }
    foreach (const Bind::ImportInfo &import, document->bind()->libraryImports()) {
        if (importContainsCursor(import.ast, cursorPosition))
            return import.ast;
    }

    CollectASTNodes nodes;
    nodes.accept(document->ast());

    foreach (AST::UiQualifiedId *q, nodes.qualifiedIds) {
        if (cursorPosition >= q->identifierToken.begin()) {
            for (AST::UiQualifiedId *tail = q; tail; tail = tail->next) {
                if (! tail->next && cursorPosition <= tail->identifierToken.end())
                    return q;
            }
        }
    }

    foreach (AST::IdentifierExpression *id, nodes.identifiers) {
        if (cursorPosition >= id->identifierToken.begin() && cursorPosition <= id->identifierToken.end())
            return id;
    }

    foreach (AST::FieldMemberExpression *mem, nodes.fieldMembers) {
        if (mem->name && cursorPosition >= mem->identifierToken.begin() && cursorPosition <= mem->identifierToken.end())
            return mem;
    }

    return 0;
}

int SemanticInfo::revision() const
{
    if (document)
        return document->editorRevision();

    return 0;
}

QmlJSEditorEditable::QmlJSEditorEditable(QmlJSTextEditor *editor)
    : BaseTextEditorEditable(editor)
{
    m_context.add(QmlJSEditor::Constants::C_QMLJSEDITOR_ID);
    m_context.add(TextEditor::Constants::C_TEXTEDITOR);
    m_context.add(QmlDesigner::Constants::C_QT_QUICK_TOOLS_MENU);
}

// Use preferred mode from Bauhaus settings
static bool openInDesignMode()
{
    static bool bauhausDetected = false;
    static bool bauhausPresent = false;
    // Check if Bauhaus is loaded, that is, a Design mode widget is
    // registered for the QML mime type.
    if (!bauhausDetected) {
        if (const Core::IMode *dm = Core::ModeManager::instance()->mode(QLatin1String(Core::Constants::MODE_DESIGN)))
            if (const Core::DesignMode *designMode = qobject_cast<const Core::DesignMode *>(dm))
                bauhausPresent = designMode->registeredMimeTypes().contains(QLatin1String(QmlJSEditor::Constants::QML_MIMETYPE));
        bauhausDetected =  true;
    }
    if (!bauhausPresent)
        return false;

    return bool(QmlDesigner::Constants::QML_OPENDESIGNMODE_DEFAULT);
}

QString QmlJSEditorEditable::preferredMode() const
{
    Core::ModeManager *modeManager = Core::ModeManager::instance();
    if (modeManager->currentMode()->id() == Core::Constants::MODE_DESIGN
        || modeManager->currentMode()->id() == Core::Constants::MODE_EDIT)
    {
        return modeManager->currentMode()->id();
    }

    // if we are in other mode than edit or design, use the hard-coded default.
    // because the editor opening decision is modal, it would be confusing to
    // have the user also access to this failsafe setting.
    if (editor()->mimeType() == QLatin1String(QmlJSEditor::Constants::QML_MIMETYPE)
        && openInDesignMode())
        return QLatin1String(Core::Constants::MODE_DESIGN);
    return QString();
}

QmlJSTextEditor::QmlJSTextEditor(QWidget *parent) :
    TextEditor::BaseTextEditor(parent),
    m_outlineCombo(0),
    m_outlineModel(new QmlOutlineModel(this)),
    m_modelManager(0),
    m_contextPane(0)
{
    qRegisterMetaType<QmlJSEditor::Internal::SemanticInfo>("QmlJSEditor::Internal::SemanticInfo");

    m_semanticHighlighter = new SemanticHighlighter(this);
    m_semanticHighlighter->start();

    setParenthesesMatchingEnabled(true);
    setMarksVisible(true);
    setCodeFoldingSupported(true);
    setCodeFoldingVisible(true);

    m_updateDocumentTimer = new QTimer(this);
    m_updateDocumentTimer->setInterval(UPDATE_DOCUMENT_DEFAULT_INTERVAL);
    m_updateDocumentTimer->setSingleShot(true);
    connect(m_updateDocumentTimer, SIGNAL(timeout()), this, SLOT(updateDocumentNow()));

    m_updateUsesTimer = new QTimer(this);
    m_updateUsesTimer->setInterval(UPDATE_USES_DEFAULT_INTERVAL);
    m_updateUsesTimer->setSingleShot(true);
    connect(m_updateUsesTimer, SIGNAL(timeout()), this, SLOT(updateUsesNow()));

    m_semanticRehighlightTimer = new QTimer(this);
    m_semanticRehighlightTimer->setInterval(UPDATE_DOCUMENT_DEFAULT_INTERVAL);
    m_semanticRehighlightTimer->setSingleShot(true);
    connect(m_semanticRehighlightTimer, SIGNAL(timeout()), this, SLOT(forceSemanticRehighlight()));

    connect(this, SIGNAL(textChanged()), this, SLOT(updateDocument()));
    connect(this, SIGNAL(textChanged()), this, SLOT(updateUses()));

    m_updateOutlineTimer = new QTimer(this);
    m_updateOutlineTimer->setInterval(UPDATE_OUTLINE_INTERVAL);
    m_updateOutlineTimer->setSingleShot(true);
    connect(m_updateOutlineTimer, SIGNAL(timeout()), this, SLOT(updateOutlineNow()));

    m_updateOutlineIndexTimer = new QTimer(this);
    m_updateOutlineIndexTimer->setInterval(UPDATE_OUTLINE_INTERVAL);
    m_updateOutlineIndexTimer->setSingleShot(true);
    connect(m_updateOutlineIndexTimer, SIGNAL(timeout()), this, SLOT(updateOutlineIndexNow()));

    baseTextDocument()->setSyntaxHighlighter(new Highlighter(document()));

    m_modelManager = ExtensionSystem::PluginManager::instance()->getObject<ModelManagerInterface>();
    m_contextPane = ExtensionSystem::PluginManager::instance()->getObject<QmlJS::IContextPane>();
    if (m_contextPane)
        connect(this, SIGNAL(cursorPositionChanged()), this, SLOT(onCursorPositionChanged()));
    m_oldCursorPosition = -1;

    if (m_modelManager) {
        m_semanticHighlighter->setModelManager(m_modelManager);
        connect(m_modelManager, SIGNAL(documentUpdated(QmlJS::Document::Ptr)),
                this, SLOT(onDocumentUpdated(QmlJS::Document::Ptr)));
        connect(this->document(), SIGNAL(modificationChanged(bool)), this, SLOT(modificationChanged(bool)));
    }

    connect(m_semanticHighlighter, SIGNAL(changed(QmlJSEditor::Internal::SemanticInfo)),
            this, SLOT(updateSemanticInfo(QmlJSEditor::Internal::SemanticInfo)));

    setRequestMarkEnabled(true);
}

QmlJSTextEditor::~QmlJSTextEditor()
{
    m_semanticHighlighter->abort();
    m_semanticHighlighter->wait();
}

SemanticInfo QmlJSTextEditor::semanticInfo() const
{
    return m_semanticInfo;
}

int QmlJSTextEditor::editorRevision() const
{
    return document()->revision();
}

bool QmlJSTextEditor::isOutdated() const
{
    if (m_semanticInfo.revision() != editorRevision())
        return true;

    return false;
}

QmlOutlineModel *QmlJSTextEditor::outlineModel() const
{
    return m_outlineModel;
}

QModelIndex QmlJSTextEditor::outlineModelIndex()
{
    if (!m_outlineModelIndex.isValid()) {
        m_outlineModelIndex = indexForPosition(position());
        emit outlineModelIndexChanged(m_outlineModelIndex);
    }
    return m_outlineModelIndex;
}

Core::IEditor *QmlJSEditorEditable::duplicate(QWidget *parent)
{
    QmlJSTextEditor *newEditor = new QmlJSTextEditor(parent);
    newEditor->duplicateFrom(editor());
    QmlJSEditorPlugin::instance()->initializeEditor(newEditor);
    return newEditor->editableInterface();
}

QString QmlJSEditorEditable::id() const
{
    return QLatin1String(QmlJSEditor::Constants::C_QMLJSEDITOR_ID);
}

bool QmlJSEditorEditable::open(const QString &fileName)
{
    bool b = TextEditor::BaseTextEditorEditable::open(fileName);
    editor()->setMimeType(Core::ICore::instance()->mimeDatabase()->findByFile(QFileInfo(fileName)).type());
    return b;
}

Core::Context QmlJSEditorEditable::context() const
{
    return m_context;
}

void QmlJSTextEditor::updateDocument()
{
    m_updateDocumentTimer->start(UPDATE_DOCUMENT_DEFAULT_INTERVAL);
}

void QmlJSTextEditor::updateDocumentNow()
{
    // ### move in the parser thread.

    m_updateDocumentTimer->stop();

    const QString fileName = file()->fileName();

    m_modelManager->updateSourceFiles(QStringList() << fileName, false);
}

static void appendExtraSelectionsForMessages(
        QList<QTextEdit::ExtraSelection> *selections,
        const QList<DiagnosticMessage> &messages,
        const QTextDocument *document)
{
    foreach (const DiagnosticMessage &d, messages) {
        const int line = d.loc.startLine;
        const int column = qMax(1U, d.loc.startColumn);

        QTextEdit::ExtraSelection sel;
        QTextCursor c(document->findBlockByNumber(line - 1));
        sel.cursor = c;

        sel.cursor.setPosition(c.position() + column - 1);

        if (d.loc.length == 0) {
            if (sel.cursor.atBlockEnd())
                sel.cursor.movePosition(QTextCursor::StartOfWord, QTextCursor::KeepAnchor);
            else
                sel.cursor.movePosition(QTextCursor::EndOfWord, QTextCursor::KeepAnchor);
        } else {
            sel.cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor, d.loc.length);
        }

        if (d.isWarning())
            sel.format.setUnderlineColor(Qt::darkYellow);
        else
            sel.format.setUnderlineColor(Qt::red);

        sel.format.setUnderlineStyle(QTextCharFormat::WaveUnderline);
        sel.format.setToolTip(d.message);

        selections->append(sel);
    }
}

void QmlJSTextEditor::onDocumentUpdated(QmlJS::Document::Ptr doc)
{
    if (file()->fileName() != doc->fileName()
            || doc->editorRevision() != document()->revision()) {
        // didn't get the currently open, or an up to date document.
        // trigger a semantic rehighlight anyway, after a time
        m_semanticRehighlightTimer->start();
        return;
    }

    if (doc->ast()) {
        // got a correctly parsed (or recovered) file.

        const SemanticHighlighter::Source source = currentSource(/*force = */ true);
        m_semanticHighlighter->rehighlight(source);
    } else {
        // show parsing errors
        QList<QTextEdit::ExtraSelection> selections;
        appendExtraSelectionsForMessages(&selections, doc->diagnosticMessages(), document());
        setExtraSelections(CodeWarningsSelection, selections);
    }
}

void QmlJSTextEditor::modificationChanged(bool changed)
{
    if (!changed && m_modelManager)
        m_modelManager->fileChangedOnDisk(file()->fileName());
}

void QmlJSTextEditor::jumpToOutlineElement(int /*index*/)
{
    QModelIndex index = m_outlineCombo->view()->currentIndex();
    AST::SourceLocation location = index.data(QmlOutlineModel::SourceLocationRole).value<AST::SourceLocation>();

    Core::EditorManager *editorManager = Core::EditorManager::instance();
    editorManager->cutForwardNavigationHistory();
    editorManager->addCurrentPositionToNavigationHistory();

    QTextCursor cursor = textCursor();
    cursor.setPosition(location.offset);
    setTextCursor(cursor);

    setFocus();
}

void QmlJSTextEditor::updateOutlineNow()
{
    if (!m_semanticInfo.document)
        return;

    if (m_semanticInfo.document->editorRevision() != editorRevision()) {
        m_updateOutlineTimer->start();
        return;
    }

    m_outlineModel->update(m_semanticInfo);

    QTreeView *treeView = static_cast<QTreeView*>(m_outlineCombo->view());
    treeView->expandAll();

    updateOutlineIndexNow();
}

void QmlJSTextEditor::updateOutlineIndexNow()
{
    if (m_updateOutlineTimer->isActive())
        return; // updateOutlineNow will call this function soon anyway

    if (!m_outlineModel->document())
        return;

    if (m_outlineModel->document()->editorRevision() != editorRevision()) {
        m_updateOutlineIndexTimer->start();
        return;
    }

    m_outlineModelIndex = QModelIndex(); // invalidate
    QModelIndex comboIndex = outlineModelIndex();

    if (comboIndex.isValid()) {
        bool blocked = m_outlineCombo->blockSignals(true);

        // There is no direct way to select a non-root item
        m_outlineCombo->setRootModelIndex(comboIndex.parent());
        m_outlineCombo->setCurrentIndex(comboIndex.row());
        m_outlineCombo->setRootModelIndex(QModelIndex());

        m_outlineCombo->blockSignals(blocked);
    }

    updateUses();
}

void QmlJSTextEditor::updateUses()
{
    m_updateUsesTimer->start();
}

void QmlJSTextEditor::updateUsesNow()
{
    if (document()->revision() != m_semanticInfo.revision()) {
        updateUses();
        return;
    }

    m_updateUsesTimer->stop();

    QList<QTextEdit::ExtraSelection> selections;
    foreach (const AST::SourceLocation &loc, m_semanticInfo.idLocations.value(wordUnderCursor())) {
        if (! loc.isValid())
            continue;

        QTextEdit::ExtraSelection sel;
        sel.format = m_occurrencesFormat;
        sel.cursor = textCursor();
        sel.cursor.setPosition(loc.begin());
        sel.cursor.setPosition(loc.end(), QTextCursor::KeepAnchor);
        selections.append(sel);
    }

    setExtraSelections(CodeSemanticsSelection, selections);

    setSelectedElements();
}

class SelectedElement: protected Visitor
{
    unsigned m_cursorPositionStart;
    unsigned m_cursorPositionEnd;
    QList<UiObjectMember *> m_selectedMembers;
    Document::Ptr m_document;
    Snapshot m_snapshot;
    LookupContext::Ptr m_lookupContext;

public:
    SelectedElement()
        : m_cursorPositionStart(0), m_cursorPositionEnd(0) {}

    QList<UiObjectMember *> operator()(Document::Ptr doc, Snapshot snapshot, unsigned startPosition, unsigned endPosition)
    {
        m_document = doc;
        m_snapshot = snapshot;
        m_lookupContext = LookupContext::create(m_document, m_snapshot, QList<Node*>());
        m_cursorPositionStart = startPosition;
        m_cursorPositionEnd = endPosition;
        m_selectedMembers.clear();
        Node::accept(doc->qmlProgram(), this);
        return m_selectedMembers;
    }

protected:

    bool isSelectable(UiObjectMember *member) const
    {
        UiQualifiedId *id = 0;
        if (UiObjectDefinition *def = cast<UiObjectDefinition *>(member))
            id = def->qualifiedTypeNameId;
        else if (UiObjectBinding *binding = cast<UiObjectBinding *>(member))
            id = binding->qualifiedTypeNameId;

        if (id) {
            QString name = id->name->asString();
            if (!name.isEmpty() && name.at(0).isUpper()) {
                return true;
            }
        }

        return false;
    }

    UiObjectInitializer *initializer(UiObjectMember *member) const
    {
        if (UiObjectDefinition *def = cast<UiObjectDefinition *>(member))
            return def->initializer;
        else if (UiObjectBinding *binding = cast<UiObjectBinding *>(member))
            return binding->initializer;
        return 0;
    }

    bool hasVisualPresentation(Node *ast)
    {
        Bind *bind = m_document->bind();
        const Interpreter::ObjectValue *objValue = bind->findQmlObject(ast);
        QStringList prototypes;

        while (objValue) {
            prototypes.append(objValue->className());
            objValue = objValue->prototype(m_lookupContext->context());
        }

        return prototypes.contains(QString("QGraphicsObject"));
    }

    bool isIdBinding(UiObjectMember *member) const
    {
        if (UiScriptBinding *script = cast<UiScriptBinding *>(member)) {
            if (! script->qualifiedId)
                return false;
            else if (! script->qualifiedId->name)
                return false;
            else if (script->qualifiedId->next)
                return false;

            const QString propertyName = script->qualifiedId->name->asString();

            if (propertyName == QLatin1String("id"))
                return true;
        }

        return false;
    }

    bool containsCursor(unsigned begin, unsigned end)
    {
        return m_cursorPositionStart >= begin && m_cursorPositionEnd <= end;
    }

    bool intersectsCursor(unsigned begin, unsigned end)
    {
        return (m_cursorPositionEnd >= begin && m_cursorPositionStart <= end);
    }

    bool isRangeSelected() const
    {
        return (m_cursorPositionStart != m_cursorPositionEnd);
    }

    virtual void postVisit(Node *ast)
    {
        if (!isRangeSelected() && !m_selectedMembers.isEmpty())
            return; // nothing to do, we already have the results.

        if (UiObjectMember *member = ast->uiObjectMemberCast()) {
            unsigned begin = member->firstSourceLocation().begin();
            unsigned end = member->lastSourceLocation().end();

            if ((isRangeSelected() && intersectsCursor(begin, end))
            || (!isRangeSelected() && containsCursor(begin, end)))
            {
                if (initializer(member) && isSelectable(member) && hasVisualPresentation(member)) {
                    m_selectedMembers << member;
                    // move start towards end; this facilitates multiselection so that root is usually ignored.
                    m_cursorPositionStart = qMin(end, m_cursorPositionEnd);
                }
            }
        }
    }
};

void QmlJSTextEditor::setSelectedElements()
{
    QTextCursor tc = textCursor();
    QString wordAtCursor;
    QList<int> offsets;

    unsigned startPos;
    unsigned endPos;

    if (tc.hasSelection()) {
        startPos = tc.selectionStart();
        endPos = tc.selectionEnd();
    } else {
        tc.movePosition(QTextCursor::StartOfWord);
        tc.movePosition(QTextCursor::EndOfWord, QTextCursor::KeepAnchor);

        startPos = textCursor().position();
        endPos = textCursor().position();
    }

    if (m_semanticInfo.document) {
        SelectedElement selectedMembers;

        QList<UiObjectMember *> members = selectedMembers(m_semanticInfo.document, m_semanticInfo.snapshot,
                                                          startPos, endPos);

        if (!members.isEmpty()) {
            foreach(UiObjectMember *m, members) {
                offsets << m->firstSourceLocation().begin();
            }
        }
    }
    wordAtCursor = tc.selectedText();

    emit selectedElementsChanged(offsets, wordAtCursor);
}

void QmlJSTextEditor::updateFileName()
{
}

void QmlJSTextEditor::renameIdUnderCursor()
{
    const QString id = wordUnderCursor();
    bool ok = false;
    const QString newId = QInputDialog::getText(Core::ICore::instance()->mainWindow(),
                                                tr("Rename..."),
                                                tr("New id:"),
                                                QLineEdit::Normal,
                                                id, &ok);
    if (ok) {
        Utils::ChangeSet changeSet;

        foreach (const AST::SourceLocation &loc, m_semanticInfo.idLocations.value(id)) {
            changeSet.replace(loc.begin(), loc.end(), newId);
        }

        QTextCursor tc = textCursor();
        changeSet.apply(&tc);
    }
}

void QmlJSTextEditor::setFontSettings(const TextEditor::FontSettings &fs)
{
    TextEditor::BaseTextEditor::setFontSettings(fs);
    Highlighter *highlighter = qobject_cast<Highlighter*>(baseTextDocument()->syntaxHighlighter());
    if (!highlighter)
        return;

    /*
        NumberFormat,
        StringFormat,
        TypeFormat,
        KeywordFormat,
        LabelFormat,
        CommentFormat,
        VisualWhitespace,
     */
    static QVector<QString> categories;
    if (categories.isEmpty()) {
        categories << QLatin1String(TextEditor::Constants::C_NUMBER)
                << QLatin1String(TextEditor::Constants::C_STRING)
                << QLatin1String(TextEditor::Constants::C_TYPE)
                << QLatin1String(TextEditor::Constants::C_KEYWORD)
                << QLatin1String(TextEditor::Constants::C_LABEL)
                << QLatin1String(TextEditor::Constants::C_COMMENT)
                << QLatin1String(TextEditor::Constants::C_VISUAL_WHITESPACE);
    }

    highlighter->setFormats(fs.toTextCharFormats(categories));
    highlighter->rehighlight();

    m_occurrencesFormat = fs.toTextCharFormat(QLatin1String(TextEditor::Constants::C_OCCURRENCES));
    m_occurrencesUnusedFormat = fs.toTextCharFormat(QLatin1String(TextEditor::Constants::C_OCCURRENCES_UNUSED));
    m_occurrencesUnusedFormat.setUnderlineStyle(QTextCharFormat::WaveUnderline);
    m_occurrencesUnusedFormat.setUnderlineColor(m_occurrencesUnusedFormat.foreground().color());
    m_occurrencesUnusedFormat.clearForeground();
    m_occurrencesUnusedFormat.setToolTip(tr("Unused variable"));
    m_occurrenceRenameFormat = fs.toTextCharFormat(QLatin1String(TextEditor::Constants::C_OCCURRENCES_RENAME));

    // only set the background, we do not want to modify foreground properties set by the syntax highlighter or the link
    m_occurrencesFormat.clearForeground();
    m_occurrenceRenameFormat.clearForeground();
}


QString QmlJSTextEditor::wordUnderCursor() const
{
    QTextCursor tc = textCursor();
    const QChar ch = characterAt(tc.position() - 1);
    // make sure that we're not at the start of the next word.
    if (ch.isLetterOrNumber() || ch == QLatin1Char('_'))
        tc.movePosition(QTextCursor::Left);
    tc.movePosition(QTextCursor::StartOfWord);
    tc.movePosition(QTextCursor::EndOfWord, QTextCursor::KeepAnchor);
    const QString word = tc.selectedText();
    return word;
}

bool QmlJSTextEditor::isElectricCharacter(QChar ch) const
{
    if (ch == QLatin1Char('}')
            || ch == QLatin1Char(']')
            || ch == QLatin1Char(':'))
        return true;
    return false;
}

bool QmlJSTextEditor::isClosingBrace(const QList<Token> &tokens) const
{

    if (tokens.size() == 1) {
        const Token firstToken = tokens.first();

        return firstToken.is(Token::RightBrace) || firstToken.is(Token::RightBracket);
    }

    return false;
}

void QmlJSTextEditor::indentBlock(QTextDocument *doc, QTextBlock block, QChar typedChar)
{
    TextEditor::TabSettings ts = tabSettings();
    QmlJSIndenter indenter;
    indenter.setTabSize(ts.m_tabSize);
    indenter.setIndentSize(ts.m_indentSize);

    const int indent = indenter.indentForBottomLine(doc->begin(), block.next(), typedChar);
    ts.indentLine(block, indent);
}

TextEditor::BaseTextEditorEditable *QmlJSTextEditor::createEditableInterface()
{
    QmlJSEditorEditable *editable = new QmlJSEditorEditable(this);
    createToolBar(editable);
    return editable;
}

void QmlJSTextEditor::createToolBar(QmlJSEditorEditable *editable)
{
    m_outlineCombo = new QComboBox;
    m_outlineCombo->setMinimumContentsLength(22);
    m_outlineCombo->setModel(m_outlineModel);

    QTreeView *treeView = new QTreeView;
    treeView->header()->hide();
    treeView->setItemsExpandable(false);
    treeView->setRootIsDecorated(false);
    m_outlineCombo->setView(treeView);
    treeView->expandAll();

    //m_outlineCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);

    // Make the combo box prefer to expand
    QSizePolicy policy = m_outlineCombo->sizePolicy();
    policy.setHorizontalPolicy(QSizePolicy::Expanding);
    m_outlineCombo->setSizePolicy(policy);

    connect(m_outlineCombo, SIGNAL(activated(int)), this, SLOT(jumpToOutlineElement(int)));
    connect(this, SIGNAL(cursorPositionChanged()), m_updateOutlineIndexTimer, SLOT(start()));

    connect(file(), SIGNAL(changed()), this, SLOT(updateFileName()));

    QToolBar *toolBar = static_cast<QToolBar*>(editable->toolBar());

    QList<QAction*> actions = toolBar->actions();
    toolBar->insertWidget(actions.first(), m_outlineCombo);
}

TextEditor::BaseTextEditor::Link QmlJSTextEditor::findLinkAt(const QTextCursor &cursor, bool /*resolveTarget*/)
{
    const SemanticInfo semanticInfo = m_semanticInfo;
    const unsigned cursorPosition = cursor.position();

    AST::Node *node = semanticInfo.nodeUnderCursor(cursorPosition);

    if (AST::UiImport *importAst = cast<AST::UiImport *>(node)) {
        // if it's a file import, link to the file
        foreach (const Bind::ImportInfo &import, semanticInfo.document->bind()->fileImports()) {
            if (import.ast == importAst) {
                BaseTextEditor::Link link(import.name);
                link.begin = importAst->firstSourceLocation().begin();
                link.end = importAst->lastSourceLocation().end();
                return link;
            }
        }
        return Link();
    }

    LookupContext::Ptr lookupContext = LookupContext::create(semanticInfo.document, semanticInfo.snapshot, semanticInfo.astPath(cursorPosition));
    const Interpreter::Value *value = lookupContext->evaluate(node);

    QString fileName;
    int line = 0, column = 0;

    if (! (value && value->getSourceLocation(&fileName, &line, &column)))
        return Link();

    BaseTextEditor::Link link;
    link.fileName = fileName;
    link.line = line;
    link.column = column - 1; // adjust the column

    if (AST::UiQualifiedId *q = AST::cast<AST::UiQualifiedId *>(node)) {
        for (AST::UiQualifiedId *tail = q; tail; tail = tail->next) {
            if (! tail->next && cursorPosition <= tail->identifierToken.end()) {
                link.begin = tail->identifierToken.begin();
                link.end = tail->identifierToken.end();
                return link;
            }
        }

    } else if (AST::IdentifierExpression *id = AST::cast<AST::IdentifierExpression *>(node)) {
        link.begin = id->firstSourceLocation().begin();
        link.end = id->lastSourceLocation().end();
        return link;

    } else if (AST::FieldMemberExpression *mem = AST::cast<AST::FieldMemberExpression *>(node)) {
        link.begin = mem->lastSourceLocation().begin();
        link.end = mem->lastSourceLocation().end();
        return link;
    }

    return Link();
}

void QmlJSTextEditor::followSymbolUnderCursor()
{
    openLink(findLinkAt(textCursor()));
}

void QmlJSTextEditor::contextMenuEvent(QContextMenuEvent *e)
{
    QMenu *menu = new QMenu();

    if (Core::ActionContainer *mcontext = Core::ICore::instance()->actionManager()->actionContainer(QmlJSEditor::Constants::M_CONTEXT)) {
        QMenu *contextMenu = mcontext->menu();
        foreach (QAction *action, contextMenu->actions())
            menu->addAction(action);
    }

    const QString id = wordUnderCursor();
    const QList<AST::SourceLocation> &locations = m_semanticInfo.idLocations.value(id);
    if (! locations.isEmpty()) {
        menu->addSeparator();
        QAction *a = menu->addAction(tr("Rename id '%1'...").arg(id));
        connect(a, SIGNAL(triggered()), this, SLOT(renameIdUnderCursor()));
    }

    appendStandardContextMenuActions(menu);

    menu->exec(e->globalPos());
    menu->deleteLater();
}

bool QmlJSTextEditor::event(QEvent *e)
{
    switch (e->type()) {
    case QEvent::ShortcutOverride:
        if (static_cast<QKeyEvent*>(e)->key() == Qt::Key_Escape && m_contextPane) {
            if (hideContextPane()) {
                e->accept();
                return true;
            }
        }
        break;
    default:
        break;
    }

    return BaseTextEditor::event(e);
}


void QmlJSTextEditor::wheelEvent(QWheelEvent *event)
{
    bool visible = false;
    if (m_contextPane && m_contextPane->widget()->isVisible())
        visible = true;

    BaseTextEditor::wheelEvent(event);

    if (visible)
        m_contextPane->apply(editableInterface(),  m_semanticInfo.document, m_semanticInfo.snapshot, m_semanticInfo.declaringMemberNoProperties(position()), true);
}

void QmlJSTextEditor::resizeEvent(QResizeEvent *event)
{
    BaseTextEditor::resizeEvent(event);
    hideContextPane();
}

 void QmlJSTextEditor::scrollContentsBy(int dx, int dy)
 {
     BaseTextEditor::scrollContentsBy(dx, dy);
     hideContextPane();
 }

void QmlJSTextEditor::unCommentSelection()
{
    Utils::unCommentSelection(this);
}

static bool isCompleteStringLiteral(const QStringRef &text)
{
    if (text.length() < 2)
        return false;

    const QChar quote = text.at(0);

    if (text.at(text.length() - 1) == quote)
        return text.at(text.length() - 2) != QLatin1Char('\\'); // ### not exactly.

    return false;
}

static Token tokenUnderCursor(const QTextCursor &cursor)
{
    const QString blockText = cursor.block().text();
    const int blockState = blockStartState(cursor.block());

    Scanner tokenize;
    const QList<Token> tokens = tokenize(blockText, blockState);
    const int pos = cursor.positionInBlock();

    int tokenIndex = 0;
    for (; tokenIndex < tokens.size(); ++tokenIndex) {
        const Token &token = tokens.at(tokenIndex);

        if (token.is(Token::Comment) || token.is(Token::String)) {
            if (pos > token.begin() && pos <= token.end())
                break;
        } else {
            if (pos >= token.begin() && pos < token.end())
                break;
        }
    }

    if (tokenIndex != tokens.size())
        return tokens.at(tokenIndex);

    return Token();
}

bool QmlJSTextEditor::contextAllowsAutoParentheses(const QTextCursor &cursor, const QString &textToInsert) const
{
    QChar ch;

    if (! textToInsert.isEmpty())
        ch = textToInsert.at(0);

    switch (ch.unicode()) {
    case '\'':
    case '"':

    case '(':
    case '[':
    case '{':

    case ')':
    case ']':
    case '}':

    case ';':
        break;

    default:
        if (ch.isNull())
            break;

        return false;
    } // end of switch

    const Token token = tokenUnderCursor(cursor);
    switch (token.kind) {
    case Token::Comment:
        return false;

    case Token::String: {
        const QString blockText = cursor.block().text();
        const QStringRef tokenText = blockText.midRef(token.offset, token.length);
        const QChar quote = tokenText.at(0);

        if (ch != quote || isCompleteStringLiteral(tokenText))
            break;

        return false;
    }

    default:
        break;
    } // end of switch

    return true;
}

bool QmlJSTextEditor::contextAllowsElectricCharacters(const QTextCursor &cursor) const
{
    Token token = tokenUnderCursor(cursor);
    switch (token.kind) {
    case Token::Comment:
    case Token::String:
        return false;
    default:
        return true;
    }
}

bool QmlJSTextEditor::isInComment(const QTextCursor &cursor) const
{
    return tokenUnderCursor(cursor).is(Token::Comment);
}

QString QmlJSTextEditor::insertMatchingBrace(const QTextCursor &tc, const QString &text, QChar, int *skippedChars) const
{
    if (text.length() != 1)
        return QString();

    if (! shouldInsertMatchingText(tc))
        return QString();

    const QChar la = characterAt(tc.position());

    const QChar ch = text.at(0);
    switch (ch.unicode()) {
    case '\'':
        if (la != ch)
            return QString(ch);
        ++*skippedChars;
        break;

    case '"':
        if (la != ch)
            return QString(ch);
        ++*skippedChars;
        break;

    case '(':
        return QString(QLatin1Char(')'));

    case '[':
        return QString(QLatin1Char(']'));

    case '{':
        return QString(); // nothing to do.

    case ')':
    case ']':
    case '}':
    case ';':
        if (la == ch)
            ++*skippedChars;
        break;

    default:
        break;
    } // end of switch

    return QString();
}

static bool shouldInsertNewline(const QTextCursor &tc)
{
    QTextDocument *doc = tc.document();
    int pos = tc.selectionEnd();

    // count the number of empty lines.
    int newlines = 0;
    for (int e = doc->characterCount(); pos != e; ++pos) {
        const QChar ch = doc->characterAt(pos);

        if (! ch.isSpace())
            break;
        else if (ch == QChar::ParagraphSeparator)
            ++newlines;
    }

    if (newlines <= 1 && doc->characterAt(pos) != QLatin1Char('}'))
        return true;

    return false;
}

QString QmlJSTextEditor::insertParagraphSeparator(const QTextCursor &tc) const
{
    if (shouldInsertNewline(tc))
        return QLatin1String("}\n");
    return QLatin1String("}");
}

void QmlJSTextEditor::forceSemanticRehighlight()
{
    m_semanticHighlighter->rehighlight(currentSource(/* force = */ true));
}

void QmlJSTextEditor::semanticRehighlight()
{
    m_semanticHighlighter->rehighlight(currentSource());
}

void QmlJSTextEditor::updateSemanticInfo(const SemanticInfo &semanticInfo)
{
    if (semanticInfo.revision() != document()->revision()) {
        // got outdated semantic info
        semanticRehighlight();
        return;
    }

    m_semanticInfo = semanticInfo;
    Document::Ptr doc = semanticInfo.document;

    // create the ranges
    CreateRanges createRanges;
    m_semanticInfo.ranges = createRanges(document(), doc);

    // Refresh the ids
    FindIdDeclarations updateIds;
    m_semanticInfo.idLocations = updateIds(doc);

    if (m_contextPane) {
        m_contextPane->setEnabled(doc->isParsedCorrectly());
    }
    if (doc->isParsedCorrectly()) {
        FindDeclarations findDeclarations;
        m_semanticInfo.declarations = findDeclarations(doc->ast());

        if (m_contextPane) {
            Node *newNode = m_semanticInfo.declaringMemberNoProperties(position());
            if (newNode) {
                m_contextPane->apply(editableInterface(), doc, m_semanticInfo.snapshot, newNode, true);
                m_oldCursorPosition = position();
            }
        }
    }

    // update outline
    m_updateOutlineTimer->start();

    // update warning/error extra selections
    QList<QTextEdit::ExtraSelection> selections;
    appendExtraSelectionsForMessages(&selections, doc->diagnosticMessages(), document());
    appendExtraSelectionsForMessages(&selections, m_semanticInfo.semanticMessages, document());
    setExtraSelections(CodeWarningsSelection, selections);
}

void QmlJSTextEditor::onCursorPositionChanged()
{


    if (m_contextPane) {
        Node *newNode = m_semanticInfo.declaringMemberNoProperties(position());
        Node *oldNode = m_semanticInfo.declaringMemberNoProperties(m_oldCursorPosition);
        if (oldNode != newNode)
            m_contextPane->apply(editableInterface(), m_semanticInfo.document, m_semanticInfo.snapshot, newNode, false);
        m_oldCursorPosition = position();
    }
}

QModelIndex QmlJSTextEditor::indexForPosition(unsigned cursorPosition, const QModelIndex &rootIndex) const
{
    QModelIndex lastIndex = rootIndex;


    const int rowCount = m_outlineModel->rowCount(rootIndex);
    for (int i = 0; i < rowCount; ++i) {
        QModelIndex childIndex = m_outlineModel->index(i, 0, rootIndex);
        AST::SourceLocation location = childIndex.data(QmlOutlineModel::SourceLocationRole).value<AST::SourceLocation>();

        if ((cursorPosition >= location.offset)
              && (cursorPosition <= location.offset + location.length)) {
            lastIndex = childIndex;
            break;
        }
    }

    if (lastIndex != rootIndex) {
        // recurse
        lastIndex = indexForPosition(cursorPosition, lastIndex);
    }
    return lastIndex;
}

bool QmlJSTextEditor::hideContextPane()
{
    bool b = (m_contextPane) && m_contextPane->widget()->isVisible();
    if (b)
        m_contextPane->apply(editableInterface(),  m_semanticInfo.document, m_semanticInfo.snapshot, 0, false);
    return b;
}

SemanticHighlighter::Source QmlJSTextEditor::currentSource(bool force)
{
    int line = 0, column = 0;
    convertPosition(position(), &line, &column);

    const Snapshot snapshot = m_modelManager->snapshot();
    const QString fileName = file()->fileName();

    QString code;
    if (force || m_semanticInfo.revision() != document()->revision())
        code = toPlainText(); // get the source code only when needed.

    const unsigned revision = document()->revision();
    SemanticHighlighter::Source source(snapshot, fileName, code,
                                       line, column, revision);
    source.force = force;
    return source;
}

SemanticHighlighter::SemanticHighlighter(QObject *parent)
        : QThread(parent),
          m_done(false),
          m_modelManager(0)
{
}

SemanticHighlighter::~SemanticHighlighter()
{
}

void SemanticHighlighter::abort()
{
    QMutexLocker locker(&m_mutex);
    m_done = true;
    m_condition.wakeOne();
}

void SemanticHighlighter::rehighlight(const Source &source)
{
    QMutexLocker locker(&m_mutex);
    m_source = source;
    m_condition.wakeOne();
}

bool SemanticHighlighter::isOutdated()
{
    QMutexLocker locker(&m_mutex);
    const bool outdated = ! m_source.fileName.isEmpty() || m_done;
    return outdated;
}

void SemanticHighlighter::run()
{
    setPriority(QThread::IdlePriority);

    forever {
        m_mutex.lock();

        while (! (m_done || ! m_source.fileName.isEmpty()))
            m_condition.wait(&m_mutex);

        const bool done = m_done;
        const Source source = m_source;
        m_source.clear();

        m_mutex.unlock();

        if (done)
            break;

        const SemanticInfo info = semanticInfo(source);

        if (! isOutdated()) {
            m_mutex.lock();
            m_lastSemanticInfo = info;
            m_mutex.unlock();

            emit changed(info);
        }
    }
}

SemanticInfo SemanticHighlighter::semanticInfo(const Source &source)
{
    m_mutex.lock();
    const int revision = m_lastSemanticInfo.revision();
    m_mutex.unlock();

    Snapshot snapshot;
    Document::Ptr doc;

    if (! source.force && revision == source.revision) {
        m_mutex.lock();
        snapshot = m_lastSemanticInfo.snapshot;
        doc = m_lastSemanticInfo.document;
        m_mutex.unlock();
    }

    if (! doc) {
        snapshot = source.snapshot;
        doc = snapshot.documentFromSource(source.code, source.fileName);
        doc->setEditorRevision(source.revision);
        doc->parse();
    }

    SemanticInfo semanticInfo;
    semanticInfo.snapshot = snapshot;
    semanticInfo.document = doc;

    QStringList importPaths;
    if (m_modelManager)
        importPaths = m_modelManager->importPaths();
    Check checker(doc, snapshot, importPaths);
    semanticInfo.semanticMessages = checker();

    return semanticInfo;
}

void SemanticHighlighter::setModelManager(QmlJS::ModelManagerInterface *modelManager)
{
    m_modelManager = modelManager;
}

