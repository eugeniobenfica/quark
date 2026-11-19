# shapes

## tests/doc/shapes.qk

### interface Shaped

```quark
pub interface Shaped { fn area() -> Float }
```

Anything with an area: a shape <of any kind> & size.

### enum Shape

```quark
pub enum Shape { Circle(radius: Float), Box(width: Float, height: Float), Empty }
```

A shape: round, or a box.

Empty has no area.

### const sides

```quark
pub const sides = 4
```

How many sides a box has.

### fn area

```quark
pub fn area(s: Shape) -> Float
```

The area of a shape, worked out from its kind.

### fn scaled

```quark
pub fn scaled(s: Shape, k: Float = 2.0) -> Shape
```

Scales a shape by k (2 when it is not given).

## tests/doc/geometry.qk

### type Point

```quark
pub type Point { x: Float, y: Float }
```

Plane geometry: points and how far apart they are.

#### method Point.distance

```quark
pub fn Point.distance(self, other: Point) -> Float
```

The distance between two points.

