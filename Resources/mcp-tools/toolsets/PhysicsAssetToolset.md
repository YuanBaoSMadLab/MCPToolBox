# PhysicsToolsets.PhysicsAssetToolset

> 17 个工具 | 更新时间: 2026-06-26 11:30:56

## `SetSphere`

**完整名称:** `PhysicsToolsets.PhysicsAssetToolset.SetSphere`

**描述:** 在形体上添加或替换Sphere碰撞图元。
如果形体上已存在拥有给定名称的任何形状，则先将其移除。

### 输入参数

- `physicsAsset` (object) **必需**: 要修改的物理资产。
- `boneName` (string) **必需**: 要修改其形体的骨骼的名称。
- `shapeName` (string) **必需**: 在形体上唯一标识此形状的名称。
- `center` (object) **必需**: 骨骼本地空间中球体的中心（厘米）。
- `radius` (number) **必需**: Sphere的半径（厘米）。必须大于零。

### 调用代码

```json
{
  "toolset_name": "PhysicsToolsets.PhysicsAssetToolset",
  "tool_name": "SetSphere",
  "arguments": {}
}
```

---

## `SetConstraintLimits`

**完整名称:** `PhysicsToolsets.PhysicsAssetToolset.SetConstraintLimits`

**描述:** 更新现有约束的角限制。

### 输入参数

- `physicsAsset` (object) **必需**: 要修改的物理资产。
- `info` (object) **必需**: 约束描述符。Bone1Name和Bone2Name标识约束。

### 调用代码

```json
{
  "toolset_name": "PhysicsToolsets.PhysicsAssetToolset",
  "tool_name": "SetConstraintLimits",
  "arguments": {}
}
```

---

## `SetCapsule`

**完整名称:** `PhysicsToolsets.PhysicsAssetToolset.SetCapsule`

**描述:** 在形体上添加或替换胶囊体碰撞图元。
如果形体上已存在拥有给定名称的任何形状，则先将其移除。
应用旋转之后，胶囊体的长轴是其局部Z轴。

### 输入参数

- `physicsAsset` (object) **必需**: 要修改的物理资产。
- `boneName` (string) **必需**: 要修改其形体的骨骼的名称。
- `shapeName` (string) **必需**: 在形体上唯一标识此形状的名称。
- `center` (object) **必需**: 胶囊体在骨骼局部空间的中心（厘米）。
- `rotation` (object) **必需**: 胶囊体在骨骼局部空间中的朝向。
- `radius` (number) **必需**: 胶囊体末端的半径（厘米）。必须大于零。
- `length` (number) **必需**: 圆柱形部分的长度（厘米）。必须为非负值。 胶囊体总高度 = 长度 + 2 * 半径。

### 调用代码

```json
{
  "toolset_name": "PhysicsToolsets.PhysicsAssetToolset",
  "tool_name": "SetCapsule",
  "arguments": {}
}
```

---

## `SetBox`

**完整名称:** `PhysicsToolsets.PhysicsAssetToolset.SetBox`

**描述:** 在形体上添加或替换盒体碰撞图元。
如果形体上已存在拥有给定名称的任何形状，则先将其移除。

### 输入参数

- `physicsAsset` (object) **必需**: 要修改的物理资产。
- `boneName` (string) **必需**: 要修改其形体的骨骼的名称。
- `shapeName` (string) **必需**: 在形体上唯一标识此形状的名称。
- `center` (object) **必需**: 盒体在骨骼局部空间的中心（厘米）。
- `rotation` (object) **必需**: 盒体在骨骼局部空间中的方向。
- `extentX` (number) **必需**: 沿本地X轴的完整范围（厘米）。必须大于零。
- `extentY` (number) **必需**: 沿局部Y轴的完整范围（厘米）。必须大于零。
- `extentZ` (number) **必需**: 沿局部Z轴的完整范围（厘米）。必须大于零。

### 调用代码

```json
{
  "toolset_name": "PhysicsToolsets.PhysicsAssetToolset",
  "tool_name": "SetBox",
  "arguments": {}
}
```

---

## `SetBodyPhysicsMode`

**完整名称:** `PhysicsToolsets.PhysicsAssetToolset.SetBodyPhysicsMode`

**描述:** 设置给定形体的物理模拟模式。

### 输入参数

- `physicsAsset` (object) **必需**: 要修改的物理资产。
- `boneName` (string) **必需**: 要修改其形体的骨骼的名称。
- `mode` (string) **必需**: 所需的模拟模式。 可选值: `Default`, `Kinematic`, `Simulated`

### 调用代码

```json
{
  "toolset_name": "PhysicsToolsets.PhysicsAssetToolset",
  "tool_name": "SetBodyPhysicsMode",
  "arguments": {}
}
```

---

## `SetBodyMassScale`

**完整名称:** `PhysicsToolsets.PhysicsAssetToolset.SetBodyMassScale`

**描述:** 设置给定形体的质量比例乘数。

### 输入参数

- `physicsAsset` (object) **必需**: 要修改的物理资产。
- `boneName` (string) **必需**: 要修改其形体的骨骼的名称。
- `massScale` (number) **必需**: 应用于计算质量的乘数。必须大于零。

### 调用代码

```json
{
  "toolset_name": "PhysicsToolsets.PhysicsAssetToolset",
  "tool_name": "SetBodyMassScale",
  "arguments": {}
}
```

---

## `RemoveShape`

**完整名称:** `PhysicsToolsets.PhysicsAssetToolset.RemoveShape`

**描述:** 按名称从形体中移除碰撞图元。

### 输入参数

- `physicsAsset` (object) **必需**: 要修改的物理资产。
- `boneName` (string) **必需**: 要修改其形体的骨骼的名称。
- `shapeName` (string) **必需**: 要移除的形状的名称。

### 调用代码

```json
{
  "toolset_name": "PhysicsToolsets.PhysicsAssetToolset",
  "tool_name": "RemoveShape",
  "arguments": {}
}
```

---

## `RemoveConstraint`

**完整名称:** `PhysicsToolsets.PhysicsAssetToolset.RemoveConstraint`

**描述:** 移除两个形体之间的约束。

### 输入参数

- `physicsAsset` (object) **必需**: 要修改的物理资产。
- `bone1Name` (string) **必需**: 子骨骼的名称。
- `bone2Name` (string) **必需**: 父骨骼的名称。

### 调用代码

```json
{
  "toolset_name": "PhysicsToolsets.PhysicsAssetToolset",
  "tool_name": "RemoveConstraint",
  "arguments": {}
}
```

---

## `RemoveBody`

**完整名称:** `PhysicsToolsets.PhysicsAssetToolset.RemoveBody`

**描述:** 移除给定骨骼的形体以及引用它的所有约束。
如果PhysicsAsset为空或BoneName不存在形体，则引发脚本错误。

### 输入参数

- `physicsAsset` (object) **必需**: 要修改的物理资产。
- `boneName` (string) **必需**: 要移除其形体的骨骼的名称。

### 调用代码

```json
{
  "toolset_name": "PhysicsToolsets.PhysicsAssetToolset",
  "tool_name": "RemoveBody",
  "arguments": {}
}
```

---

## `GetConstraints`

**完整名称:** `PhysicsToolsets.PhysicsAssetToolset.GetConstraints`

**描述:** 返回物理资产中的所有约束及其当前角限制。

### 输入参数

- `physicsAsset` (object) **必需**: 要查询的物理资产。

### 调用代码

```json
{
  "toolset_name": "PhysicsToolsets.PhysicsAssetToolset",
  "tool_name": "GetConstraints",
  "arguments": {}
}
```

---

## `GetBodyShapes`

**完整名称:** `PhysicsToolsets.PhysicsAssetToolset.GetBodyShapes`

**描述:** 返回分配给形体的所有碰撞形状。

### 输入参数

- `physicsAsset` (object) **必需**: 要查询的物理资产。
- `boneName` (string) **必需**: 要检索其形体形状的骨骼的名称。

### 调用代码

```json
{
  "toolset_name": "PhysicsToolsets.PhysicsAssetToolset",
  "tool_name": "GetBodyShapes",
  "arguments": {}
}
```

---

## `GetBodyPhysicsMode`

**完整名称:** `PhysicsToolsets.PhysicsAssetToolset.GetBodyPhysicsMode`

**描述:** 返回给定形体的物理模拟模式。

### 输入参数

- `physicsAsset` (object) **必需**: 要查询的物理资产。
- `boneName` (string) **必需**: 要查询其形体的骨骼的名称。

### 调用代码

```json
{
  "toolset_name": "PhysicsToolsets.PhysicsAssetToolset",
  "tool_name": "GetBodyPhysicsMode",
  "arguments": {}
}
```

---

## `GetBodyNames`

**完整名称:** `PhysicsToolsets.PhysicsAssetToolset.GetBodyNames`

**描述:** 返回物理资产中每个刚体的骨骼名称。

### 输入参数

- `physicsAsset` (object) **必需**: 要查询的物理资产。

### 调用代码

```json
{
  "toolset_name": "PhysicsToolsets.PhysicsAssetToolset",
  "tool_name": "GetBodyNames",
  "arguments": {}
}
```

---

## `GetBodyMassScale`

**完整名称:** `PhysicsToolsets.PhysicsAssetToolset.GetBodyMassScale`

**描述:** 返回给定形体的质量缩放倍数。

### 输入参数

- `physicsAsset` (object) **必需**: 要查询的物理资产。
- `boneName` (string) **必需**: 要查询其形体的骨骼的名称。

### 调用代码

```json
{
  "toolset_name": "PhysicsToolsets.PhysicsAssetToolset",
  "tool_name": "GetBodyMassScale",
  "arguments": {}
}
```

---

## `CreateFromMesh`

**完整名称:** `PhysicsToolsets.PhysicsAssetToolset.CreateFromMesh`

**描述:** 从骨骼网格体创建物理资产，为每个骨骼自动生成
碰撞体。资产放置在与网格体相同的文件夹中，后缀为
“_PhysicsAsset”。

### 输入参数

- `meshPath` (string) **必需**: 骨骼网格体的内容浏览器路径，例如：“/Game/Characters/SKM_Hero”。
- `bAssignToMesh` (boolean) **必需**: 如为true，则将新物理资产分配给网格体。

### 调用代码

```json
{
  "toolset_name": "PhysicsToolsets.PhysicsAssetToolset",
  "tool_name": "CreateFromMesh",
  "arguments": {}
}
```

---

## `AddConstraint`

**完整名称:** `PhysicsToolsets.PhysicsAssetToolset.AddConstraint`

**描述:** 在两个形体之间添加一个新约束。两个形体必须已经存在。

### 输入参数

- `physicsAsset` (object) **必需**: 要修改的物理资产。
- `bone1Name` (string) **必需**: 子骨骼的名称。
- `bone2Name` (string) **必需**: 父骨骼的名称。

### 调用代码

```json
{
  "toolset_name": "PhysicsToolsets.PhysicsAssetToolset",
  "tool_name": "AddConstraint",
  "arguments": {}
}
```

---

## `AddBody`

**完整名称:** `PhysicsToolsets.PhysicsAssetToolset.AddBody`

**描述:** 为给定骨骼添加一个新的空形体。

### 输入参数

- `physicsAsset` (object) **必需**: 要修改的物理资产。
- `boneName` (string) **必需**: 要为其添加形体的骨骼的名称。

### 调用代码

```json
{
  "toolset_name": "PhysicsToolsets.PhysicsAssetToolset",
  "tool_name": "AddBody",
  "arguments": {}
}
```

---
