#pragma once

#include "ast/ast.hpp"
#include "lexer/token.hpp"
#include "diagnostics/diagnostics_engine.hpp"
#include "parser/expr_parser.hpp"
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// Recursive-descent parser for Folio (.folio files).

namespace folio {

    class Parser {
    public:
        Parser(std::vector<Token> tokens, DiagnosticsEngine& engine);

        // document := page_decl node_decl* ;
        Document parse();

    private:
        // The shared precedence climber drives the token stream directly.
        friend Expr parseExprPrecedence(Parser&, const ExprPolicy&, int);

        std::vector<Token> tokens;
        DiagnosticsEngine& engine;
        std::size_t position = 0;

        // The two expression grammars share one precedence climber (expr_parser.hpp);
        // each just says how to parse an operand.
        ExprPolicy exprPolicy;
        ExprPolicy shaderPolicy;

        // token stream helper functions 
        const Token& peek(std::size_t offset = 0) const;
        const Token& previous() const;
        const Token& advance();
        bool check(TokenKind kind) const;
        bool checkKeyword(std::string_view keyword) const; 
        bool match(TokenKind kind);
        bool matchKeyword(std::string_view keyword);
        const Token& expect(TokenKind kind, std::string_view message);
        bool isAtEnd() const;

        // for error recovery.
        // advance until a likely statement or prop boundary (';', '}') 
        // or a recognised prop/keyword starts after a parse error.
        void synchronize();

        /* Basic grammer parsing functions */

        PageDecl parsePageDecl();
        void parsePageProp(PageDecl& page);            
        PageSize parsePageSize();                       
        Expr parseDimension();                         

        NodeDecl parseNodeDecl();                       
        NodeType parseNodeType();                           
        void parseNodeBody(NodeDecl& node);                  
        bool parseCommonProp(NodeDecl& node, const Token& propName);
        bool parseShapeProp(NodeDecl& node, const Token& propName);   
        void parseCircleProp(NodeDecl& node, const Token& propName);
        void parseTextProp(NodeDecl& node, const Token& propName);
        void parseImageProp(NodeDecl& node, const Token& propName);
        void parsePathProp(NodeDecl& node, const Token& propName);
        std::vector<PathPoint> parsePointList();             
        PathPoint parsePoint();                                
        Statement parseStatement();                             

        // expression parsing 
        Expr parseExpr();                  // expr: precedence climber over primary_num
        Expr parsePrimary();         
        Expr parseSizedNumber();
        Expr parseShaderNumber();   
        Expr parseReference();    

        Fill parseFillExpr(); // fill_expr
        Expr parseColorLiteral(bool insideShader = false);
        Fill parseGradientFill(bool isRadial); // gradient_fill
        std::vector<GradientStop> parseStopList(); // stop_list
        GradientStop parseStop();                    // stop
        Fill parseTextureFill();         // texture_fill
        std::string parseImportExpr();       // import_expr
        Fill parseShaderFill();            // shader_fill
        Statement parseShaderStatement();     // shader_stmt := IDENT ':' type '=' shader_expr ';'
        Stroke parseStrokeExpr();          // stroke_expr
        
        Expr parseShaderExpr();            // shader_expr: precedence climber over postfix_expr
        Expr parseShaderPostfix();         // postfix_expr (swizzle)
        Expr parseShaderPrimary();         // primary_expr
        Expr parseVecConstructor();        // vec_constructor
        Expr parseBuiltinCall();           // builtin_call
        std::vector<Expr> parseExprList(); // expr_list (shared by fill_expr calls and shader calls)
    };
} 
