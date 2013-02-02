// Copyright (C) 2012, Chris J. Foster and the other authors and contributors
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice,
//   this list of conditions and the following disclaimer.
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
// * Neither the name of the software's owners nor the names of its
//   contributors may be used to endorse or promote products derived from this
//   software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// (This is the BSD 3-clause license)

#ifndef SHADER_H_INCLUDED
#define SHADER_H_INCLUDED

#include <memory>

#include <QtCore/QMap>
#include <QtOpenGL/QGLShader>
#include <QtOpenGL/QGLShaderProgram>


/// Representation of a shader "parameter" (uniform variable or attribute)
struct ShaderParam
{
    enum Type {
        Float,
        Int,
        Vec3
    };

    Type type;             ///< Variable type
    QByteArray name;       ///< Name of the variable in the shader
    QVariant defaultValue; ///< Default value
    QMap<QString,QString> kvPairs; ///< name,value pairs with additional metadata

    QString uiName() const
    {
        return kvPairs.value("uiname", name);
    }

    double min() const
    {
        return kvPairs.value("min", "0").toDouble();
    }

    double max() const
    {
        return kvPairs.value("max", "100").toDouble();
    }

    ShaderParam(Type type=Float, QByteArray name="",
                QVariant defaultValue = QVariant())
        : type(type), name(name), defaultValue(defaultValue) {}
};


inline bool operator==(const ShaderParam& p1, const ShaderParam& p2)
{
    return p1.name == p2.name && p1.type == p2.type &&
        p1.defaultValue == p2.defaultValue &&
        p1.kvPairs == p2.kvPairs;
}


// Operator for ordering in QMap
inline bool operator<(const ShaderParam& p1, const ShaderParam& p2)
{
    if (p1.name != p2.name)
        return p1.name < p2.name;
    return p1.type < p2.type;
}


/// Wrapper for QGLShader, with functionality added to parse
/// the list of uniform parameters.
class Shader
{
    public:
        Shader(QGLShader::ShaderType type, const QGLContext* context)
            : m_shader(type, context)
        { }

        /// Return list of uniform shader parameters
        const QList<ShaderParam>& uniforms() const
        {
            return m_uniforms;
        }

        /// Return original source code.
        QByteArray sourceCode() const
        {
            return m_source;
        }

        /// Access to underlying shader
        QGLShader* shader()
        {
            return &m_shader;
        }

        bool compileSourceCode(const QByteArray& src);

    private:
        QList<ShaderParam> m_uniforms;
        QGLShader m_shader;  ///< Underlying shader
        QByteArray m_source; ///< Non-mangled source code
};


/// Wrapper around QGLShaderProgram providing parameter tweaking UI
///
/// When compiling a new shader, the shader source code is scanned for
/// annotations in the comments which indicate which uniform values should be
/// tweakable by the user.  An appropriate UI editor widget is automatically
/// created for each such uniform value by a call to setupParameterUI().
///
class ShaderProgram : public QObject
{
    Q_OBJECT

    public:
        ShaderProgram(const QGLContext * context, QObject* parent = 0);

        QGLShaderProgram& shaderProgram() { return *m_shaderProgram; }

        void setupParameterUI(QWidget* parentWidget);
        void setUniforms();

        QByteArray vertexShader() const;
        QByteArray fragmentShader() const;

    public slots:
        void setVertexShader(QString src);
        void setFragmentShader(QString src);

    signals:
        void paramsChanged();
        void uniformValuesChanged();

    private slots:
        void setUniformValue(int value);
        void setUniformValue(double value);

    private:
        void setupParameters();

        const QGLContext* m_context;
        double m_pointSize;
        double m_exposure;
        double m_contrast;
        int m_selector;
        typedef QMap<ShaderParam,QVariant> ParamMap;
        ParamMap m_params;
        std::unique_ptr<Shader> m_vertexShader;
        std::unique_ptr<Shader> m_fragmentShader;
        std::unique_ptr<QGLShaderProgram> m_shaderProgram;
};


#endif // SHADER_H_INCLUDED